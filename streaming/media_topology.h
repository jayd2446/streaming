#pragma once
#include "media_stream.h"
#include "presentation_clock.h"
#include <memory>
#include <unordered_map>
#include <vector>

/*
components must be multithreading safe, streams don't have to be;
components can be shared between topologies, but must not be shared between sessions;
streams shouldn't be shared between topologies because they are assumed to be singlethreaded
*/

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
    // TODO: implement topology cloning;
    // requires streams to clone themselves to a new stream

    // TODO: allow for passing a custom clock in the constructor
    media_topology();
    bool connect_streams(const media_stream_t& stream, const media_stream_t& stream2);
    presentation_clock_t get_clock() const {return this->clock;}
};

typedef std::shared_ptr<media_topology> media_topology_t;