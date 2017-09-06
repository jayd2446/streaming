#pragma once
#include "media_stream.h"
#include <memory>
#include <unordered_map>
#include <vector>

// each topology node must have a unique component associated to it

// 1 topology -> combined topology with transition transform -> 2 topology
// media session's switcher will switch seamlessly between these topologies

class media_topology
{
    friend class media_session;
public:
    struct topology_node
    {
        std::vector<media_stream_t> next;
    };
    typedef std::unordered_map<const media_stream*, topology_node> topology_t;
private:
    topology_t topology, topology_reverse;
public:
    //bool has_node(const media_topology_node_t& node) const;
    //// returns false if the node already exists
    //bool add_node(const media_topology_node_t& node);

    bool connect_streams(const media_stream_t& stream, const media_stream_t& stream2);

    //// used to break the circular dependency between nodes and this
    //bool shutdown();
    //bool is_shutdown() const;
};

typedef std::shared_ptr<media_topology> media_topology_t;