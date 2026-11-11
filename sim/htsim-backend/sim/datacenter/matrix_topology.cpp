#include "matrix_topology.h"
#include "compositequeue.h"
#include "main.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <queue>
#include <sstream>
#include <stdexcept>

namespace {

std::string trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> fields;
    const char delimiter = line.find(',') == std::string::npos ? ' ' : ',';
    std::stringstream stream(line);
    std::string field;
    if (delimiter == ' ') {
        while (stream >> field) fields.push_back(trim(field));
    } else {
        while (std::getline(stream, field, delimiter)) fields.push_back(trim(field));
    }
    return fields;
}

bool parseUnsigned(const std::string& value, uint32_t& result) {
    const std::string cleaned = trim(value);
    if (cleaned.empty()) return false;
    char* end = NULL;
    const unsigned long parsed = std::strtoul(cleaned.c_str(), &end, 10);
    if (end == cleaned.c_str() || *end != '\0' || parsed > UINT32_MAX) return false;
    result = static_cast<uint32_t>(parsed);
    return true;
}

std::string jsonValue(const std::string& object, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t position = object.find(needle);
    if (position == std::string::npos) return std::string();
    position = object.find(':', position + needle.size());
    if (position == std::string::npos) return std::string();
    ++position;
    while (position < object.size() && std::isspace(static_cast<unsigned char>(object[position]))) ++position;
    size_t end = position;
    while (end < object.size() && object[end] != ',' && object[end] != '}') ++end;
    return trim(object.substr(position, end - position));
}

bool parseJsonRoutes(const std::string& contents,
                     std::map<std::pair<uint32_t, uint32_t>,
                              std::vector<std::vector<uint32_t> > >& routes) {
    size_t position = 0;
    bool found_object = false;
    while ((position = contents.find('{', position)) != std::string::npos) {
        const size_t end = contents.find('}', position + 1);
        if (end == std::string::npos) return false;
        const std::string object = contents.substr(position, end - position + 1);
        uint32_t src = 0, dst = 0;
        if (jsonValue(object, "src").empty() || jsonValue(object, "dst").empty()) {
            position = end + 1;
            continue;
        }
        if (!parseUnsigned(jsonValue(object, "src"), src) ||
            !parseUnsigned(jsonValue(object, "dst"), dst)) return false;
        found_object = true;

        std::vector<std::vector<uint32_t> > paths;
        const size_t paths_key = object.find("\"paths\"");
        if (paths_key != std::string::npos) {
            size_t cursor = object.find('[', paths_key);
            if (cursor == std::string::npos) return false;
            int depth = 0;
            std::vector<uint32_t> path;
            while (cursor < object.size()) {
                const char c = object[cursor];
                if (c == '[') {
                    ++depth;
                    if (depth == 2) path.clear();
                } else if (c == ']') {
                    if (depth == 2 && !path.empty()) paths.push_back(path);
                    --depth;
                    if (depth == 0) break;
                } else if (depth == 2 && (std::isdigit(static_cast<unsigned char>(c)))) {
                    size_t number_end = cursor;
                    while (number_end < object.size() &&
                           std::isdigit(static_cast<unsigned char>(object[number_end]))) ++number_end;
                    uint32_t node = 0;
                    if (!parseUnsigned(object.substr(cursor, number_end - cursor), node)) return false;
                    path.push_back(node);
                    cursor = number_end - 1;
                }
                ++cursor;
            }
        }
        routes[std::make_pair(src, dst)] = paths;
        position = end + 1;
    }
    return found_object;
}

bool parseDelimitedRoutes(const std::string& contents,
                          std::map<std::pair<uint32_t, uint32_t>,
                                   std::vector<std::vector<uint32_t> > >& routes) {
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields = splitFields(line);
        if (fields.size() < 3) continue; // header or empty route
        uint32_t src = 0, dst = 0;
        if (!parseUnsigned(fields[0], src) || !parseUnsigned(fields[1], dst)) continue;

        std::vector<uint32_t> path;
        // Accept a path cell such as 0->1->2, 0;1;2, or 0 1 2.
        std::string path_text;
        for (size_t i = 2; i < fields.size(); ++i) {
            if (!path_text.empty()) path_text += ' ';
            path_text += fields[i];
        }
        for (size_t i = 0; i < path_text.size();) {
            if (!std::isdigit(static_cast<unsigned char>(path_text[i]))) {
                ++i;
                continue;
            }
            size_t end = i;
            while (end < path_text.size() && std::isdigit(static_cast<unsigned char>(path_text[end]))) ++end;
            uint32_t node = 0;
            if (!parseUnsigned(path_text.substr(i, end - i), node)) return false;
            path.push_back(node);
            i = end;
        }
        std::vector<std::vector<uint32_t> > paths;
        if (!path.empty()) paths.push_back(path);
        routes[std::make_pair(src, dst)] = paths;
    }
    return !routes.empty();
}

}

MatrixSwitch::MatrixSwitch(EventList& eventlist, const std::string& name,
                           uint32_t server_id, simtime_picosec switch_latency,
                           MatrixTopology* topology)
    : Switch(eventlist, name), _server_id(server_id), _topology(topology),
      _switch_pipe(new CallbackPipe(switch_latency, eventlist, this)) {}

uint64_t MatrixSwitch::hostRouteKey(uint32_t rank, uint32_t flow_id) {
    return (static_cast<uint64_t>(rank) << 32) | flow_id;
}

void MatrixSwitch::addOutgoing(uint32_t peer, BaseQueue* queue, Pipe* pipe) {
    std::shared_ptr<Route> route(new Route());
    route->push_back(queue);
    route->push_back(pipe);
    route->push_back(_topology->_switches[peer]);
    EdgePort edge = {queue, pipe, route};
    _outgoing[peer].push_back(edge);
}

void MatrixSwitch::addHostPort(uint32_t rank, uint32_t flow_id, PacketSink* transport, bool local) {
    std::shared_ptr<Route> route(new Route());
    if (!local) {
        route->push_back(_topology->_host_egress_queues[_server_id]);
        route->push_back(_topology->_host_egress_pipes[_server_id]);
    }
    route->push_back(transport);
    _host_routes[hostRouteKey(rank, flow_id)] = route;
}

void MatrixSwitch::installFlow(uint32_t flow_id, const MatrixFlowRoute& route) {
    _flow_routes[flow_id] = route;
}

Route* MatrixSwitch::getNextHop(Packet& pkt, BaseQueue*) {
    const std::unordered_map<uint32_t, MatrixFlowRoute>::const_iterator flow_it =
        _flow_routes.find(pkt.flow_id());
    if (flow_it == _flow_routes.end()) {
        std::cerr << "No matrix route registered for flow " << pkt.flow_id()
                  << " at server " << _server_id << std::endl;
        std::abort();
    }

    const MatrixFlowRoute& flow = flow_it->second;
    const uint32_t destination_server = _topology->serverForRank(pkt.dst());
    if (destination_server == _server_id) {
        const uint64_t key = hostRouteKey(pkt.dst(), pkt.flow_id());
        const std::unordered_map<uint64_t, std::shared_ptr<Route> >::const_iterator host_it =
            _host_routes.find(key);
        if (host_it == _host_routes.end()) {
            std::cerr << "No local endpoint route for rank " << pkt.dst()
                      << " and flow " << pkt.flow_id() << " on server " << _server_id
                      << std::endl;
            std::abort();
        }
        return host_it->second.get();
    }

    const std::vector<uint32_t>* path = NULL;
    if (pkt.dst() == flow.dst_rank) path = &flow.forward;
    else if (pkt.dst() == flow.src_rank) path = &flow.reverse;
    if (path == NULL) {
        std::cerr << "Packet destination " << pkt.dst() << " does not match registered flow "
                  << flow.src_rank << "->" << flow.dst_rank << std::endl;
        std::abort();
    }

    const std::vector<uint32_t>::const_iterator current =
        std::find(path->begin(), path->end(), _server_id);
    if (current == path->end() || current + 1 == path->end()) {
        std::cerr << "Global route does not continue from server " << _server_id
                  << " toward rank " << pkt.dst() << std::endl;
        std::abort();
    }
    const uint32_t peer = *(current + 1);
    const std::unordered_map<uint32_t, std::vector<EdgePort> >::const_iterator edge_it =
        _outgoing.find(peer);
    if (edge_it == _outgoing.end() || edge_it->second.empty()) {
        std::cerr << "No physical matrix link from server " << _server_id
                  << " to server " << peer << std::endl;
        std::abort();
    }
    const std::vector<EdgePort>& lanes = edge_it->second;
    const EdgePort& edge = lanes[pkt.flow_id() % lanes.size()];
    return edge.route.get();
}

void MatrixSwitch::receivePacket(Packet& pkt) {
    if (_packets_in_switch.insert(&pkt).second) {
        Route* next = getNextHop(pkt, NULL);
        pkt.set_route(*next);
        const MatrixFlowRoute& flow = _flow_routes.at(pkt.flow_id());
        if (_topology->serverForRank(flow.src_rank) ==
            _topology->serverForRank(flow.dst_rank)) {
            // The local fabric already supplied serialization and propagation
            // delay. Do not charge OCS forwarding latency or NIC bandwidth.
            _packets_in_switch.erase(&pkt);
            pkt.sendOn();
            return;
        }
        _switch_pipe->receivePacket(pkt);
    } else {
        _packets_in_switch.erase(&pkt);
        pkt.sendOn();
    }
}

MatrixTopology::MatrixTopology(EventList& eventlist, linkspeed_bps link_speed,
                               mem_b queuesize, simtime_picosec link_latency,
                               simtime_picosec switch_latency)
    : _eventlist(&eventlist), _link_speed(link_speed), _queuesize(queuesize),
      _link_latency(link_latency), _switch_latency(switch_latency) {}

MatrixTopology* MatrixTopology::load(const char* matrix_file, const char* routes_file,
                                    const char* rank_server_map_file,
                                    EventList& eventlist, linkspeed_bps link_speed,
                                    mem_b queuesize, simtime_picosec link_latency,
                                    simtime_picosec switch_latency) {
    if (matrix_file == NULL || matrix_file[0] == '\0') {
        std::cerr << "Matrix topology mode requires -matrix <adjacency-matrix.csv|topology.json>" << std::endl;
        return NULL;
    }
    MatrixTopology* topology = new MatrixTopology(eventlist, link_speed, queuesize,
                                                  link_latency, switch_latency);
    topology->loadMatrix(matrix_file);
    if (routes_file != NULL && routes_file[0] != '\0') topology->loadRoutes(routes_file);
    else topology->buildShortestRoutes();
    if (rank_server_map_file != NULL) topology->_rank_server_map_file = rank_server_map_file;
    topology->validateRoutes();
    topology->buildNetwork();
    return topology;
}

void MatrixTopology::loadMatrix(const char* filename) {
    std::ifstream input(filename);
    if (!input) {
        throw std::runtime_error(std::string("Cannot open adjacency matrix: ") + filename);
    }

    std::vector<std::vector<uint32_t> > rows;
    std::ostringstream all_text;
    all_text << input.rdbuf();
    const std::string contents = all_text.str();
    const size_t first = contents.find_first_not_of(" \t\r\n");

    if (first != std::string::npos && contents[first] == '{') {
        uint32_t value = 0;
        const std::string local_speed = jsonValue(contents, "local_speed_mbps");
        if (!local_speed.empty()) {
            if (!parseUnsigned(local_speed, value) || value == 0)
                throw std::runtime_error("local_speed_mbps must be a positive integer");
            _local_speed = speedFromMbps(static_cast<uint64_t>(value));
        }
        const std::string local_latency = jsonValue(contents, "local_latency_ns");
        if (!local_latency.empty()) {
            if (!parseUnsigned(local_latency, value))
                throw std::runtime_error("local_latency_ns must be a nonnegative integer");
            _local_latency = timeFromNs(value);
        }
        // generator.py writes config.num_servers and a flat row-major global matrix.
        const char* keys[] = {"global", "matrix", "adjacency_matrix"};
        size_t array_start = std::string::npos;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            const std::string key = "\"" + std::string(keys[i]) + "\"";
            const size_t key_position = contents.find(key);
            if (key_position == std::string::npos) continue;
            const size_t colon = contents.find(':', key_position + key.size());
            if (colon != std::string::npos) array_start = contents.find('[', colon + 1);
            if (array_start != std::string::npos) break;
        }
        if (array_start == std::string::npos) {
            throw std::runtime_error(std::string("JSON topology has no global/matrix array: ") + filename);
        }

        std::vector<uint32_t> values;
        size_t cursor = array_start;
        int depth = 0;
        while (cursor < contents.size()) {
            const char c = contents[cursor];
            if (c == '[') {
                ++depth;
            } else if (c == ']') {
                --depth;
                if (depth == 0) break;
            } else if (depth > 0 && std::isdigit(static_cast<unsigned char>(c))) {
                size_t end = cursor;
                while (end < contents.size() && std::isdigit(static_cast<unsigned char>(contents[end]))) ++end;
                uint32_t value = 0;
                if (!parseUnsigned(contents.substr(cursor, end - cursor), value)) {
                    throw std::runtime_error("JSON adjacency matrix contains an invalid value");
                }
                values.push_back(value);
                cursor = end - 1;
            }
            ++cursor;
        }

        uint32_t n = 0;
        const std::string configured_size = jsonValue(contents, "num_servers");
        if (!configured_size.empty() && !parseUnsigned(configured_size, n)) {
            throw std::runtime_error("JSON topology config.num_servers must be a positive integer");
        }
        if (n == 0) n = static_cast<uint32_t>(std::sqrt(static_cast<double>(values.size())));
        if (n == 0 || static_cast<uint64_t>(n) * n != values.size()) {
            throw std::runtime_error("JSON matrix element count must equal num_servers squared");
        }
        for (size_t row = 0; row < n; ++row) {
            rows.push_back(std::vector<uint32_t>(values.begin() + row * n,
                                                  values.begin() + (row + 1) * n));
        }
    } else {
        std::istringstream lines(contents);
        std::string line;
        bool header_seen = false;
        while (std::getline(lines, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> fields = splitFields(line);
            if (fields.empty()) continue;
            if (!header_seen && !fields[0].empty() && !std::isdigit(static_cast<unsigned char>(fields[0][0]))) {
                header_seen = true;
                continue;
            }
            std::vector<uint32_t> values;
            for (size_t i = 0; i < fields.size(); ++i) {
                uint32_t value = 0;
                if (!parseUnsigned(fields[i], value)) {
                    throw std::runtime_error(std::string("Invalid matrix value in ") + filename + ": " + fields[i]);
                }
                values.push_back(value);
            }
            if (!values.empty()) rows.push_back(values);
        }

        if (header_seen && !rows.empty()) {
            // CSV files from the topology generators include the row index as the
            // first field (src/dst,0,1,...). Strip it and verify row ordering.
            for (size_t row = 0; row < rows.size(); ++row) {
                if (rows[row].size() != rows.size() + 1 || rows[row][0] != row) {
                    throw std::runtime_error("Matrix CSV rows must have an index column followed by N values");
                }
                rows[row].erase(rows[row].begin());
            }
        } else if (rows.size() == 1) {
            const size_t n = static_cast<size_t>(std::sqrt(static_cast<double>(rows[0].size())));
            if (n * n != rows[0].size()) {
                throw std::runtime_error("Flattened adjacency matrix must contain a perfect-square number of values");
            }
            std::vector<uint32_t> values = rows[0];
            rows.clear();
            for (size_t i = 0; i < n; ++i) {
                rows.push_back(std::vector<uint32_t>(values.begin() + i * n,
                                                     values.begin() + (i + 1) * n));
            }
        }
    }

    if (rows.empty()) throw std::runtime_error(std::string("Adjacency matrix is empty: ") + filename);
    const size_t n = rows.size();
    for (size_t row = 0; row < n; ++row) {
        if (rows[row].size() != n) {
            throw std::runtime_error("Adjacency matrix must be square (N rows by N columns)");
        }
        if (rows[row][row] != 0) {
            throw std::runtime_error("Adjacency matrix diagonal must be zero; self-links are not modeled");
        }
    }
    _matrix.swap(rows);
    uint64_t lane_count = 0;
    for (size_t row = 0; row < _matrix.size(); ++row) {
        lane_count = std::accumulate(_matrix[row].begin(), _matrix[row].end(), lane_count);
    }
    std::cout << "Loaded directed server adjacency matrix: " << _matrix.size()
              << " servers, " << lane_count << " directed link lanes" << std::endl;
}

void MatrixTopology::loadRoutes(const char* filename) {
    std::ifstream input(filename);
    if (!input) throw std::runtime_error(std::string("Cannot open global route table: ") + filename);
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string text = contents.str();
    const size_t first = text.find_first_not_of(" \t\r\n");
    bool parsed = false;
    if (first != std::string::npos && (text[first] == '[' || text[first] == '{')) {
        parsed = parseJsonRoutes(text, _routes);
    }
    if (!parsed) parsed = parseDelimitedRoutes(text, _routes);
    if (!parsed) throw std::runtime_error(std::string("No route entries found in ") + filename);
    std::cout << "Loaded global route table: " << _routes.size() << " ordered pairs"
              << std::endl;
}

void MatrixTopology::buildShortestRoutes() {
    const uint32_t n = _matrix.size();
    for (uint32_t src = 0; src < n; ++src) {
        std::vector<int32_t> parent(n, -1);
        std::queue<uint32_t> pending;
        parent[src] = static_cast<int32_t>(src);
        pending.push(src);
        while (!pending.empty()) {
            const uint32_t node = pending.front();
            pending.pop();
            for (uint32_t dst = 0; dst < n; ++dst) {
                if (_matrix[node][dst] == 0 || parent[dst] != -1) continue;
                parent[dst] = static_cast<int32_t>(node);
                pending.push(dst);
            }
        }
        for (uint32_t dst = 0; dst < n; ++dst) {
            if (src == dst || parent[dst] == -1) continue;
            std::vector<uint32_t> path;
            for (uint32_t node = dst; node != src; node = static_cast<uint32_t>(parent[node])) path.push_back(node);
            path.push_back(src);
            std::reverse(path.begin(), path.end());
            _routes[std::make_pair(src, dst)].push_back(path);
        }
    }
    std::cout << "No route file supplied; generated deterministic shortest paths from the matrix"
              << std::endl;
}

void MatrixTopology::validateRoutes() const {
    const uint32_t n = _matrix.size();
    for (std::map<std::pair<uint32_t, uint32_t>,
                  std::vector<std::vector<uint32_t> > >::const_iterator entry = _routes.begin();
         entry != _routes.end(); ++entry) {
        const uint32_t src = entry->first.first;
        const uint32_t dst = entry->first.second;
        if (src >= n || dst >= n) throw std::runtime_error("Route table endpoint is outside the matrix range");
        for (size_t p = 0; p < entry->second.size(); ++p) {
            const std::vector<uint32_t>& path = entry->second[p];
            if (path.size() < 2 || path.front() != src || path.back() != dst) {
                throw std::runtime_error("Each inter-server route must begin at src and end at dst");
            }
            std::set<uint32_t> visited;
            for (size_t i = 0; i < path.size(); ++i) {
                if (path[i] >= n) throw std::runtime_error("Route contains a server outside the matrix range");
                if (!visited.insert(path[i]).second) throw std::runtime_error("Route paths cannot contain loops");
                if (i + 1 < path.size() && _matrix[path[i]][path[i + 1]] == 0) {
                    throw std::runtime_error("Route table uses a link missing from the adjacency matrix");
                }
            }
        }
    }
}

std::vector<uint32_t> MatrixTopology::chooseRoute(uint32_t src, uint32_t dst) const {
    if (src == dst) return std::vector<uint32_t>(1, src);
    const std::map<std::pair<uint32_t, uint32_t>,
                    std::vector<std::vector<uint32_t> > >::const_iterator route_it =
        _routes.find(std::make_pair(src, dst));
    if (route_it == _routes.end() || route_it->second.empty()) {
        std::ostringstream message;
        message << "No global route for server pair " << src << "->" << dst;
        throw std::runtime_error(message.str());
    }
    const size_t choice = static_cast<size_t>(::random()) % route_it->second.size();
    return route_it->second[choice];
}

void MatrixTopology::configureRankMapping(uint32_t rank_count) {
    if (rank_count == 0) throw std::runtime_error("GOAL trace contains no ranks");
    std::vector<uint32_t> mapping;
    if (!_rank_server_map_file.empty()) {
        std::ifstream input(_rank_server_map_file.c_str());
        if (!input) throw std::runtime_error("Cannot open rank-to-server map file: " + _rank_server_map_file);
        std::string line;
        while (std::getline(input, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const std::vector<std::string> fields = splitFields(line);
            for (size_t i = 0; i < fields.size(); ++i) {
                uint32_t server = 0;
                if (!parseUnsigned(fields[i], server)) {
                    throw std::runtime_error("Rank-to-server map must contain only server IDs");
                }
                mapping.push_back(server);
            }
        }
        if (mapping.size() != rank_count) {
            throw std::runtime_error("Rank-to-server map must contain exactly one server ID per GOAL rank");
        }
        for (size_t rank = 0; rank < mapping.size(); ++rank) {
            if (mapping[rank] >= _matrix.size()) {
                throw std::runtime_error("Rank-to-server map refers to a server outside the matrix range");
            }
        }
    } else {
        if (rank_count % _matrix.size() != 0) {
            throw std::runtime_error("GOAL rank count must be divisible by server count; provide -rank_server_map for an explicit placement");
        }
        const uint32_t ranks_per_server = rank_count / _matrix.size();
        for (uint32_t rank = 0; rank < rank_count; ++rank) {
            mapping.push_back(rank / ranks_per_server);
        }
    }
    _rank_to_server.swap(mapping);
    std::cout << "Configured GOAL rank placement: " << rank_count << " ranks over "
              << _matrix.size() << " servers" << std::endl;
}

uint32_t MatrixTopology::serverForRank(uint32_t rank) const {
    if (rank >= _rank_to_server.size()) {
        std::cerr << "GOAL rank " << rank << " has no server placement; check the trace rank count"
                  << std::endl;
        std::abort();
    }
    return _rank_to_server[rank];
}

Route* MatrixTopology::hostToSwitchRoute(uint32_t rank, bool local) const {
    const uint32_t server = serverForRank(rank);
    return local ? _local_routes[server].get() : _host_ingress_routes[server].get();
}

void MatrixTopology::addHostPort(uint32_t rank, uint32_t flow_id, PacketSink* transport, bool local) {
    _switches[serverForRank(rank)]->addHostPort(rank, flow_id, transport, local);
}

void MatrixTopology::registerFlow(uint32_t flow_id, uint32_t src_rank, uint32_t dst_rank) {
    const uint32_t src_server = serverForRank(src_rank);
    const uint32_t dst_server = serverForRank(dst_rank);
    MatrixFlowRoute flow;
    flow.src_rank = src_rank;
    flow.dst_rank = dst_rank;
    flow.forward = chooseRoute(src_server, dst_server);
    flow.reverse = chooseRoute(dst_server, src_server);
    std::set<uint32_t> participants(flow.forward.begin(), flow.forward.end());
    participants.insert(flow.reverse.begin(), flow.reverse.end());
    for (std::set<uint32_t>::const_iterator it = participants.begin(); it != participants.end(); ++it) {
        _switches[*it]->installFlow(flow_id, flow);
    }
}

std::vector<const Route*>* MatrixTopology::get_bidir_paths(uint32_t, uint32_t, bool) {
    // Routes are selected per trace flow and installed in MatrixSwitch. The
    // legacy path-enumeration API is not used by the GOAL adapter.
    return new std::vector<const Route*>();
}

std::vector<uint32_t>* MatrixTopology::get_neighbours(uint32_t src) {
    if (src >= _matrix.size()) return NULL;
    std::vector<uint32_t>* neighbours = new std::vector<uint32_t>();
    for (uint32_t dst = 0; dst < _matrix.size(); ++dst) {
        if (_matrix[src][dst] > 0) neighbours->push_back(dst);
    }
    return neighbours;
}

void MatrixTopology::buildNetwork() {
    const uint32_t n = _matrix.size();
    _switches.resize(n, NULL);
    _host_ingress_queues.resize(n, NULL);
    _host_ingress_pipes.resize(n, NULL);
    _host_egress_queues.resize(n, NULL);
    _host_egress_pipes.resize(n, NULL);
    _host_ingress_routes.resize(n);
    _local_routes.resize(n);

    for (uint32_t server = 0; server < n; ++server) {
        std::ostringstream name;
        name << "OCS_Server_" << server;
        _switches[server] = new MatrixSwitch(*_eventlist, name.str(), server,
                                             _switch_latency, this);
    }

    for (uint32_t server = 0; server < n; ++server) {
        uint64_t tx_lanes = 0, rx_lanes = 0;
        for (uint32_t peer = 0; peer < n; ++peer) {
            tx_lanes += _matrix[server][peer];
            rx_lanes += _matrix[peer][server];
        }
        const linkspeed_bps tx_rate = _link_speed * std::max<uint64_t>(1, tx_lanes);
        const linkspeed_bps rx_rate = _link_speed * std::max<uint64_t>(1, rx_lanes);
        std::ostringstream name;
        name << "Server_" << server << "_NIC_TX";
        FairPriorityQueue* ingress = new FairPriorityQueue(tx_rate, _queuesize, *_eventlist, NULL);
        ingress->setName(name.str());
        ingress->setRemoteEndpoint(_switches[server]);
        _host_ingress_queues[server] = ingress;
        _host_ingress_pipes[server] = new Pipe(_link_latency, *_eventlist);
        std::shared_ptr<Route> host_route(new Route());
        host_route->push_back(_host_ingress_queues[server]);
        host_route->push_back(_host_ingress_pipes[server]);
        host_route->push_back(_switches[server]);
        _host_ingress_routes[server] = host_route;

        FairPriorityQueue* local_queue = new FairPriorityQueue(
            _local_speed, _queuesize, *_eventlist, NULL);
        local_queue->setName("Server_" + std::to_string(server) + "_LOCAL");
        local_queue->setRemoteEndpoint(_switches[server]);
        std::shared_ptr<Route> local_route(new Route());
        local_route->push_back(local_queue);
        local_route->push_back(new Pipe(_local_latency, *_eventlist));
        local_route->push_back(_switches[server]);
        _local_routes[server] = local_route;

        std::ostringstream output_name;
        output_name << "Server_" << server << "_NIC_RX";
        CompositeQueue* egress = new CompositeQueue(rx_rate, _queuesize, *_eventlist, NULL);
        egress->setName(output_name.str());
        _switches[server]->addPort(egress);
        _host_egress_queues[server] = egress;
        _host_egress_pipes[server] = new Pipe(_link_latency, *_eventlist);
    }

    for (uint32_t src = 0; src < n; ++src) {
        for (uint32_t dst = 0; dst < n; ++dst) {
            for (uint32_t lane = 0; lane < _matrix[src][dst]; ++lane) {
                std::ostringstream name;
                name << "OCS_" << src << "_to_" << dst << "_lane_" << lane;
                CompositeQueue* queue = new CompositeQueue(_link_speed, _queuesize,
                                                            *_eventlist, NULL);
                queue->setName(name.str());
                queue->setRemoteEndpoint(_switches[dst]);
                _switches[src]->addPort(queue);
                Pipe* pipe = new Pipe(_link_latency, *_eventlist);
                _switches[src]->addOutgoing(dst, queue, pipe);
            }
        }
    }
    std::cout << "Built Htsim matrix network: " << n << " server switches, link rate "
              << _link_speed / 1000000000 << " Gbps per lane" << std::endl;
    std::cout << "Intra-server shared fabric: " << _local_speed / 1000000000
              << " Gbps, " << _local_latency / 1000 << " ns; no OCS NIC usage" << std::endl;
}
