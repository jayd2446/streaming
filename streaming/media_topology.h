#pragma once
#include "media_stream.h"
#include "presentation_clock.h"
#include <memory>
#include <unordered_map>
#include <vector>

/*
components must be multithreading safe, streams don't have to be;
components can be shared between topologies, but must not be shared between sessions;
streams must not be shared between topologies because they are assumed to be singlethreaded
and to ensure there aren't overlapping rps;

only one topology is active in a session at a time

if changing the time source, new session must be created,
which also means that components need to be reinitialized
*/

// TODO: improve topology traverse speed

class media_topology
{
    friend class media_session;
public:
    struct topology_node {std::vector<media_stream_t> next;};
    typedef std::unordered_map<const media_stream*, topology_node> topology_t;
private:
    topology_t topology, topology_reverse;
    presentation_clock_t clock;
    volatile int packet_number, first_packet_number;
public:
    // TODO: implement topology cloning;
    // requires streams to clone themselves to a new stream
    explicit media_topology(const presentation_time_source_t&);

    bool connect_streams(const media_stream_t& stream, const media_stream_t& stream2);
    presentation_clock_t get_clock() const {return this->clock;}
    int get_first_packet_number() const {return this->first_packet_number;}
};

typedef std::shared_ptr<media_topology> media_topology_t;