#pragma once
#include "media_clock.h"
#include <memory>
#include <unordered_map>
#include <vector>

/*
components and streams must be multithreading safe;
components can only be shared between successive topologies;
streams must not be shared between topologies and sessions

multiple topologies might be active in a session for a brief moment after switching topologies

if changing the time source, new session must be created,
which also means that components need to be reinitialized
*/

class media_stream;
typedef std::shared_ptr<media_stream> media_stream_t;

// TODO: improve topology traverse speed

// singlethreaded

class media_topology
{
    friend class media_session;
    friend class media_stream;
public:
    struct topology_node {std::vector<media_stream_t> next;};
    typedef std::unordered_map<const media_stream*, topology_node> topology_t;
private:
    std::vector<media_stream_t> source_streams;
    topology_t topology, topology_reverse;
    media_message_generator_t message_generator;
    volatile int next_packet_number;
    int topology_number;

    // only one request stream connection is added for a node;
    // subsequent connections are discarded;
    // called by media_stream only
    void connect_streams(const media_stream_t& stream, const media_stream_t& stream2);
public:
    // media session uses this value to determine whether the drain operation for the topology
    // has completed
    bool drained;

    explicit media_topology(const media_message_generator_t&);

    media_message_generator_t get_message_generator() const {return this->message_generator;}
    int get_topology_number() const {return this->topology_number;}
};

typedef std::shared_ptr<media_topology> media_topology_t;