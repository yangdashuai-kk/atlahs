#ifndef ATLAHS_MATRIX_TOPOLOGY_H
#define ATLAHS_MATRIX_TOPOLOGY_H

#include "topology.h"
#include "switch.h"
#include "queue.h"
#include "pipe.h"
#include "callback_pipe.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

class MatrixTopology;

struct MatrixFlowRoute {
    uint32_t src_rank;
    uint32_t dst_rank;
    std::vector<uint32_t> forward;
    std::vector<uint32_t> reverse;
};

class MatrixSwitch : public Switch {
public:
    MatrixSwitch(EventList& eventlist, const std::string& name, uint32_t server_id,
                 simtime_picosec switch_latency, MatrixTopology* topology);

    void addOutgoing(uint32_t peer, BaseQueue* queue, Pipe* pipe);
    void addHostPort(uint32_t rank, uint32_t flow_id, PacketSink* transport, bool local);
    void installFlow(uint32_t flow_id, const MatrixFlowRoute& route);
    virtual Route* getNextHop(Packet& pkt, BaseQueue* ingress_port);
    virtual void receivePacket(Packet& pkt);

private:
    struct EdgePort {
        BaseQueue* queue;
        Pipe* pipe;
        std::shared_ptr<Route> route;
    };

    static uint64_t hostRouteKey(uint32_t rank, uint32_t flow_id);
    uint32_t _server_id;
    MatrixTopology* _topology;
    CallbackPipe* _switch_pipe;
    std::unordered_map<uint32_t, std::vector<EdgePort> > _outgoing;
    std::unordered_map<uint64_t, std::shared_ptr<Route> > _host_routes;
    std::unordered_map<uint32_t, MatrixFlowRoute> _flow_routes;
    std::set<Packet*> _packets_in_switch;
};

// A server-level directed adjacency matrix backed by real Htsim queues and
// switches. Matrix values are parallel directed links, not route weights.
class MatrixTopology : public Topology {
public:
    static MatrixTopology* load(const char* matrix_file, const char* routes_file,
                                const char* rank_server_map_file,
                                EventList& eventlist, linkspeed_bps link_speed,
                                mem_b queuesize, simtime_picosec link_latency,
                                simtime_picosec switch_latency);

    virtual std::vector<const Route*>* get_bidir_paths(uint32_t src, uint32_t dest,
                                                        bool reverse);
    virtual std::vector<uint32_t>* get_neighbours(uint32_t src);
    virtual uint32_t no_of_nodes() const { return _matrix.size(); }

    uint32_t serverForRank(uint32_t rank) const;
    void configureRankMapping(uint32_t rank_count);
    Route* hostToSwitchRoute(uint32_t rank, bool local = false) const;
    void addHostPort(uint32_t rank, uint32_t flow_id, PacketSink* transport, bool local = false);
    void registerFlow(uint32_t flow_id, uint32_t src_rank, uint32_t dst_rank);
    uint32_t rankCount() const { return _rank_to_server.size(); }

private:
    friend class MatrixSwitch;
    MatrixTopology(EventList& eventlist, linkspeed_bps link_speed,
                   mem_b queuesize, simtime_picosec link_latency,
                   simtime_picosec switch_latency);
    void loadMatrix(const char* filename);
    void loadRoutes(const char* filename);
    void buildShortestRoutes();
    void validateRoutes() const;
    std::vector<uint32_t> chooseRoute(uint32_t src, uint32_t dst) const;
    void buildNetwork();

    EventList* _eventlist;
    linkspeed_bps _link_speed;
    mem_b _queuesize;
    simtime_picosec _link_latency;
    simtime_picosec _switch_latency;
    // Simplified shared intra-server fabric, independent of the OCS NICs.
    linkspeed_bps _local_speed = 1200000000000ULL;
    simtime_picosec _local_latency = 0;
    std::string _rank_server_map_file;
    std::vector<std::vector<uint32_t> > _matrix;
    std::map<std::pair<uint32_t, uint32_t>,
             std::vector<std::vector<uint32_t> > > _routes;
    std::vector<uint32_t> _rank_to_server;
    std::vector<MatrixSwitch*> _switches;
    std::vector<HostQueue*> _host_ingress_queues;
    std::vector<Pipe*> _host_ingress_pipes;
    std::vector<BaseQueue*> _host_egress_queues;
    std::vector<Pipe*> _host_egress_pipes;
    std::vector<std::shared_ptr<Route> > _host_ingress_routes;
    std::vector<std::shared_ptr<Route> > _local_routes;
};

#endif
