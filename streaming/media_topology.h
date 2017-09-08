#pragma once
#include "media_stream.h"
#include "presentation_clock.h"
#include <memory>
#include <unordered_map>
#include <vector>

// each topology node must have a unique component associated to it
// ^ for transitions to work correctly components must be shared between topologies

// 1 topology -> combined topology with transition transform -> 2 topology
// media session's switcher will switch seamlessly between these topologies

// TODO: probably should be renamed to scene
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
    presentation_clock_t clock;
public:
    media_topology();
    bool connect_streams(const media_stream_t& stream, const media_stream_t& stream2);
    presentation_clock_t get_clock() const {return this->clock;}
};

typedef std::shared_ptr<media_topology> media_topology_t;