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
#include <atomic>

// orchestrates the data flow between components in the media pipeline

class media_stream;

class media_session : public enable_shared_from_this
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    presentation_time_source_t time_source;

    std::recursive_mutex topology_switch_mutex;

    media_topology_t current_topology;
    media_topology_t new_topology;

    // starts the new topology immediately;
    // throws if the topology doesn't include a clock;
    // throws also if the session has been shutdown
    void switch_topology_immediate(const media_topology_t& new_topology, time_unit time_point);
public:
    explicit media_session(const presentation_time_source_t&);
    ~media_session();

    bool get_current_topology(media_topology_t&) const;
    // returns the clock of the current topology;
    // components shouldn't store the reference because it might
    // create a cyclic dependency between clock and components;
    // returns false if the clock is NULL
    bool get_current_clock(presentation_clock_t&) const;
    const presentation_time_source_t& get_time_source() const {return this->time_source;}

    void switch_topology(const media_topology_t& new_topology);
    // throws if the topology doesn't include a clock
    void start_playback(const media_topology_t& topology, time_unit time_point);
    // even though media session provides stop playback functions,
    // they generally shouldn't be used since the components assume that
    // start/stop time points match the request time points(that assumption
    // is the basis for component draining)
    /*void stop_playback();
    void stop_playback(time_unit);*/

    // playback is stopped by switching to an empty topology
    /*void stop_playback();
    void stop_playback(time_unit);*/

    // TODO: make request_sample call that's coming from sink
    // its own function;
    // make give_sample that is coming from the sink its own function aswell

    // TODO: subsequent request sample and give calls must not fail;
    // failing might lead to deadlocks in sink

    // TODO: use same return values for media streams and media session

    // request_sample returns false if the stream isn't found on the active topology;
    // request_sample won't fail when the is_sink is false
    // TODO: remove is_sink&is_source flags
    // TODO: make request_packet& const
    bool request_sample(
        const media_stream* this_input_stream, 
        request_packet&,
        bool is_sink);
    // is_source flag is used for the media session to able to translate the sample times;
    // TODO: give sample must not fail
    bool give_sample(
        const media_stream* this_output_stream, 
        const media_sample& sample_view,
        request_packet&,
        bool is_source);

    // begins and completes the request_sample call chain and handles topology switching;
    // returns the topology that was used for the chain
    media_topology_t begin_request_sample(const media_stream* this_stream, const request_packet&);
    // begins the give_sample call chain
    void begin_give_sample(const media_stream* this_stream, const media_topology_t&);

    // breaks the circular dependency between components and the session;
    // the session is considered invalid after calling this;
    // the components bound to this session must be reinitialized aswell
    /*void shutdown();*/
};

typedef std::shared_ptr<media_session> media_session_t;