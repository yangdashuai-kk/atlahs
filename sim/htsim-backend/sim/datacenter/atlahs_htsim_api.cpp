#include "atlahs_htsim_api.h"
#include "eqds.h"
#include "atlahs_event.h"
#include "fat_tree_topology.h"
#include "matrix_topology.h"
#include "logsim-interface.h"
#include "../lgs/LogGOPSim.hpp"
#include <fstream>
#include <set>
#include <stdexcept>
    
bool AtlahsHtsimApi::llama_rand = false;

void AtlahsHtsimApi::loadNativeGoalServerMap(const std::string& filename) {
    if (!_topo || _matrix_topo) throw std::runtime_error("Native GOAL server map requires -topo");
    std::ifstream input(filename);
    if (!input) throw std::runtime_error("Cannot read native GOAL server map: " + filename);
    std::vector<uint32_t> mapping;
    std::set<uint32_t> seen;
    long long server;
    while (input >> server) {
        if (server < 0 || server >= _topo->no_of_nodes() || !seen.insert(server).second)
            throw std::runtime_error("Native GOAL map must use distinct valid server IDs");
        mapping.push_back(static_cast<uint32_t>(server));
    }
    if (!input.eof() || mapping.empty())
        throw std::runtime_error("Native GOAL map must contain whitespace-separated integers");
    native_goal_server_map.swap(mapping);
}

void AtlahsHtsimApi::setGoalRankMappingFromBinaryHeader(uint32_t rank_count,
                                                       int cpu_count, int nic_count) {
    setNumberNic(nic_count);
    const bool looks_like_v2_gpu_rank =
        nic_count == 1 ||
        (nic_count == 2 && rank_count > static_cast<uint32_t>(nic_count) && cpu_count <= 8);
    goal_rank_mapping =
        looks_like_v2_gpu_rank ? GoalRankMapping::GpuRank : GoalRankMapping::UniqueNic;
    if (!native_goal_server_map.empty()) {
        if (native_goal_server_map.size() != rank_count)
            throw std::runtime_error("Native GOAL map length differs from GOAL rank count");
        for (uint32_t server : native_goal_server_map) {
            const uint64_t end = (static_cast<uint64_t>(server) + 1) *
                (usesUniqueNicRankMapping() ? nic_count : 1);
            if (end > static_cast<uint64_t>(_topo->no_of_nodes()))
                throw std::runtime_error("Native topology has too few NIC endpoints for GOAL placement");
        }
    }
    if (_matrix_topo) _matrix_topo->configureRankMapping(rank_count);
}

int AtlahsHtsimApi::getHtsimNodeNumber(int lgs_host, int lgs_nic) {
    if (_matrix_topo) return _matrix_topo->serverForRank(static_cast<uint32_t>(lgs_host));
    if (!native_goal_server_map.empty()) {
        const int server = native_goal_server_map.at(lgs_host);
        return usesUniqueNicRankMapping() ? server * number_nics + lgs_nic : server;
    }
    return usesUniqueNicRankMapping() ? lgs_host * number_nics + lgs_nic : lgs_host;
}

void AtlahsHtsimApi::Send(const SendEvent &event, graph_node_properties elem) {
    //std::cout << "AtlahsHtsimApi: Sending event" << std::endl;

    int to = event.getTo();
    int from = event.getFrom();
    int tag = event.getTag();
    int size = event.getSizeBytes();
    size = size * 1;

    if (!_matrix_topo) {
        from = getHtsimNodeNumber(from, elem.nic);
        to = getHtsimNodeNumber(to, elem.nic);
    }

    const uint32_t server_from = _matrix_topo
        ? _matrix_topo->serverForRank(static_cast<uint32_t>(from))
        : static_cast<uint32_t>(from);
    const uint32_t server_to = _matrix_topo
        ? _matrix_topo->serverForRank(static_cast<uint32_t>(to))
        : static_cast<uint32_t>(to);

    const bool local = _matrix_topo && server_from == server_to;
    // MatrixSwitch owns global path/lane selection. UEC/NDP only need one
    // transport path-ID slot here; 128 unused slots per flow accumulate large
    // amounts of memory over a multi-million-operation GOAL replay.
    const uint32_t transport_paths = goal_transport_paths ? goal_transport_paths : (_matrix_topo ? 1 : 128);
    const auto host_route = [this, local](int endpoint) -> Route* {
        if (_matrix_topo) return _matrix_topo->hostToSwitchRoute(static_cast<uint32_t>(endpoint), local);
        const uint32_t node = static_cast<uint32_t>(endpoint);
        const uint32_t tor = _topo->HOST_POD_SWITCH(node);
        Route* route = new Route();
        route->push_back(_topo->queues_ns_nlp[node][tor][0]);
        route->push_back(_topo->pipes_ns_nlp[node][tor][0]);
        route->push_back(_topo->queues_ns_nlp[node][tor][0]->getRemoteEndpoint());
        return route;
    };
    const auto add_host_port = [this, local](int endpoint, uint32_t flow_id, PacketSink* transport) {
        if (_matrix_topo) {
            _matrix_topo->addHostPort(static_cast<uint32_t>(endpoint), flow_id, transport, local);
        } else {
            const uint32_t node = static_cast<uint32_t>(endpoint);
            _topo->switches_lp[_topo->HOST_POD_SWITCH(node)]->addHostPort(node, flow_id, transport);
        }
    };
    const auto install_matrix_flow = [this, from, to](uint32_t flow_id) {
        if (_matrix_topo) _matrix_topo->registerFlow(flow_id, static_cast<uint32_t>(from),
                                                     static_cast<uint32_t>(to));
    };

    // Temporary solution
    if (llama_rand && !_matrix_topo) {
        if (from >= 4) {
            from = from + 12;
            to = to + 12;
        }
    }
    
    if (from == to) {
        std::cerr << "Error: Send event from and to the same node" << std::endl;
        exit(0);
    }

    if (_logsim_interface->get_protocol() == EQDS_PROTOCOL) {
        EqdsSrc* eqds_src;
        EqdsSink* eqds_snk;
        eqds_src = new EqdsSrc(NULL, *_eventlist, *nics.at(server_from));
        //eqds_src->setCwnd(initial_cwnd);
        //eqds_srcs.push_back(eqds_src);
        eqds_src->setDst(to);
        /* printf("Setting up connection from %d to %d\n", from, to);
        printf("Size Pacers %d\n", pacers.size());
        printf("Size NICs %d\n", nics.size()); */
        eqds_snk = new EqdsSink(NULL,pacersEQDS[server_to],*nics.at(server_to));
        eqds_src->setName("Eqds_" + ntoa(from) + "_" + ntoa(to));
        eqds_snk->setSrc(from);
                        
        eqds_snk->setName("Eqds_sink_" + ntoa(from) + "_" + ntoa(to));
        eqds_src->setFlowsize(size);

        Route* srctotor = host_route(from);
        Route* dsttotor = host_route(to);

        eqds_src->from = from;
        eqds_src->to = to;
        eqds_src->tag = tag;
        eqds_src->_atlahs_api = this;

        graph_node_properties* node_copy = new graph_node_properties(elem);
        eqds_src->lgs_node = node_copy;

        eqds_src->connect(*srctotor, *dsttotor, *eqds_snk, _eventlist->now());
        //eqds_src->setPaths(path_entropy_size);
        //eqds_snk->setPaths(path_entropy_size);

        //register src and snk to receive packets from their respective TORs. 
        add_host_port(from, eqds_snk->flowId(), eqds_src);
        add_host_port(to, eqds_src->flowId(), eqds_snk);
        install_matrix_flow(eqds_src->flowId());
    } else if (_logsim_interface->get_protocol() == NDP_PROTOCOL) {
        NdpSrc* ndpSrc;
        NdpSink* ndpSnk;
        ndpSrc = new NdpSrc(NULL, NULL, *_eventlist, false);
        ndpSrc->setCwnd(50*Packet::data_packet_size());
        ndpSrc->set_dst(to);
        ndpSrc->set_flowsize(size);

        ndpSnk = new NdpSink(pacersNDP[server_to]);
        ndpSnk->set_src(from);
                        
        ndpSnk->setName("ndp_sink_" + ntoa(from) + "_" + ntoa(to));
        ndpSrc->setName("ndp_" + ntoa(from) + "_" + ntoa(to));

        Route* srctotor = host_route(from);
        Route* dsttotor = host_route(to);

        ndpSrc->from = from;
        ndpSrc->to = to;
        ndpSrc->tag = tag;
        ndpSnk->from_sink = from;
        ndpSnk->to_sink = to;
        ndpSnk->tag_sink = tag;
        ndpSrc->_atlahs_api = this;

        graph_node_properties* node_copy = new graph_node_properties(elem);
        ndpSrc->lgs_node = node_copy;
        ndpSrc->connect(srctotor, dsttotor, *ndpSnk, _eventlist->now());
        ndpSrc->set_paths(transport_paths);
        ndpSnk->set_paths(transport_paths);

        //register src and snk to receive packets from their respective TORs. 
        add_host_port(from, ndpSrc->flow_id(), ndpSrc);
        add_host_port(to, ndpSrc->flow_id(), ndpSnk);
        install_matrix_flow(ndpSrc->flow_id());
    } else if (_logsim_interface->get_protocol() == SENDER_PROTOCOL) { 

        UecSrc *uecSrc = new UecSrc(NULL, NULL, *_eventlist, getSenderRtt(), getSenderBdp(), 100, 6);

        uecSrc->setFlowSize(size);

        uecSrc->setName("uec_" + std::to_string(from) + "_" + std::to_string(to));
        uecSrc->from = from;
        uecSrc->to = to;
        uecSrc->tag = tag;
        uecSrc->send_size = size;
        uecSrc->_atlahs_api = this;

        UecSink *uecSink = new UecSink();
        uecSink->setName("uec_sink_Rand");
        uecSink->from_sink = from;
        uecSink->to_sink = to;
        uecSink->tag_sink = tag;

        uecSrc->set_dst(to);
        uecSink->set_src(from);

        Route* srctotor = host_route(from);
        Route* dsttotor = host_route(to);

        graph_node_properties* node_copy = new graph_node_properties(elem);
        uecSrc->lgs_node = node_copy;
        uecSrc->connect(srctotor, dsttotor, *uecSink, _eventlist->now());

        uecSrc->set_paths(transport_paths);
        uecSink->set_paths(transport_paths);

        //register src and snk to receive packets from their respective TORs. 
        add_host_port(from, uecSrc->flow_id(), uecSrc);
        add_host_port(to, uecSrc->flow_id(), uecSink);
        install_matrix_flow(uecSrc->flow_id());

    }
    // TODO: Move this stuff to a CreateConnection function inside UEC. 
    // TODO: Support different tranports, not just UEC
}

void AtlahsHtsimApi::Recv(const RecvEvent &event) {
    // No Op for HTSIM
}

void AtlahsHtsimApi::Calc(const ComputeAtlahsEvent &event) {
    // Done Directly in lgs_interface for now
}

void AtlahsHtsimApi::Setup() {
    printf("No of nodes %d\n", total_nodes);

    if (_logsim_interface->get_protocol() == EQDS_PROTOCOL) {
        for (size_t ix = 0; ix < total_nodes; ix++){
            printf("Setting up node %d\n", ix);
            pacersEQDS.push_back(new EqdsPullPacer(linkspeed, 0.99, EqdsSrc::_mtu, *_eventlist));   
            nics.push_back(new EqdsNIC(*_eventlist, linkspeed));
        }
    } else if (_logsim_interface->get_protocol() == NDP_PROTOCOL) {
        for (size_t ix = 0; ix < total_nodes; ix++)
            pacersNDP.push_back(new NdpPullPacer(*_eventlist,  linkspeed, 0.99));   
    }
    
}

void AtlahsHtsimApi::EventFinished(const EventOver &event) {
    //std::cout << "AtlahsHtsimApi: Event is over" << std::endl;

    if (AtlahsEventType::SEND_EVENT_OVER == event.getEventType()) {
        //_logsim_interface->flow_over(*(event.getPacket()));
        _logsim_interface->flow_over(event);
    } else if (AtlahsEventType::COMPUTE_EVENT_OVER == event.getEventType()) {
        _logsim_interface->compute_over(1);
    } else {
        abort();
    }
}
