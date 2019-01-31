#pragma once
#include "media_sample.h"
#include "media_topology.h"
#include "presentation_clock.h"
#include "async_callback.h"
#include "enable_shared_from_this.h"
#include "request_packet.h"
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <queue>

// orchestrates the data flow between components in the media pipeline

class media_stream;

class media_session : public enable_shared_from_this
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    presentation_time_source_t time_source;

    std::mutex request_chain_mutex;
    std::unique_lock<std::mutex> request_chain_lock;

    media_topology_t current_topology;
    media_topology_t new_topology;

    // starts the new topology immediately;
    // throws if the topology doesn't include a clock
    void switch_topology_immediate(const media_topology_t& new_topology, time_unit time_point);
public:
    explicit media_session(const presentation_time_source_t&);
    ~media_session();

    // the function throws if it is called from other function than on_stream_start/on_stream_stop
    // and the component counterparts or request_sample
    media_topology_t get_current_topology() const;
    const presentation_time_source_t& get_time_source() const {return this->time_source;}

    void switch_topology(const media_topology_t& new_topology);
    // throws if the topology doesn't include a clock;
    // playback is stopped by switching to an empty topology
    void start_playback(const media_topology_t& topology, time_unit time_point);

    // TODO: subsequent request sample and give calls must not fail;
    // failing might lead to deadlocks in sink

    // TODO: use same return values for media streams and media session

    // request_sample returns false if the stream isn't found on the active topology
    bool request_sample(
        const media_stream* this_input_stream, 
        const request_packet&);
    // TODO: give sample must not fail
    // args can be null
    bool give_sample(
        const media_stream* this_output_stream, 
        const media_component_args* args,
        const request_packet&);

    // begins and completes the request_sample call chain and handles topology switching;
    // the request call chain is atomic;
    // begin_request_sample calls the sink_stream request_sample
    bool begin_request_sample(media_stream* sink_stream, const request_packet&);
    // begins the give_sample call chain;
    // begin_give_sample calls the streams connected to sink_stream
    void begin_give_sample(const media_stream* sink_stream, const media_topology_t&);
};

typedef std::shared_ptr<media_session> media_session_t;