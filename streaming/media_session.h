#pragma once
#include "media_sample.h"
#include "media_topology.h"
#include "media_clock.h"
#include "async_callback.h"
#include "enable_shared_from_this.h"
#include "request_packet.h"
#include <memory>
#include <mutex>

// orchestrates the data flow between components in the media pipeline

class media_stream;

class media_session final : public enable_shared_from_this
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    media_clock_t time_source;

    std::mutex request_chain_mutex;
    std::unique_lock<std::mutex> request_chain_lock;

    media_topology_t current_topology;
    media_topology_t new_topology;

    // starts the new topology immediately;
    // throws if the topology doesn't include an event generator
    void switch_topology_immediate(const media_topology_t& new_topology, time_unit time_point);
public:
    const frame_unit frame_rate_num, frame_rate_den;

    media_session(const media_clock_t&, frame_unit frame_rate_num, frame_unit frame_rate_den);
    
    // the function throws if it is called from other function than on_stream_start/on_stream_stop
    // and the component counterparts or request_sample
    // (throws if the request_chain_lock is not held)
    media_topology_t get_current_topology() const;
    const media_clock_t& get_clock() const {return this->time_source;}

    void switch_topology(const media_topology_t& new_topology);
    // throws if the topology doesn't include a clock;
    // playback is stopped by switching to an empty topology
    void start_playback(const media_topology_t& topology, time_unit time_point);

    // TODO: subsequent request sample and give calls must not fail;
    // failing might lead to deadlocks in sink

    // TODO: use same return values for media streams and media session

    // request_sample returns false on fatal_error
    bool request_sample(
        const media_stream* this_input_stream, 
        const request_packet&);
    // TODO: give sample must not fail
    // args can be null;
    // when give_sample has been called, the caller has processed the request and must not
    // perform any further processing(unless the stream stays locked);
    // failure to comply causes more requests to be active than the set limit, which will lead
    // to uncontrolled memory allocations and effectively leaks;
    // if give_sample is called while holding the stream lock, care must be taken to make sure
    // that no other locks are being held
    bool give_sample(
        const media_stream* this_output_stream, 
        const media_component_args* args,
        const request_packet&);

    // begins and completes the request_sample call chain and handles topology switching;
    // the request call chain is atomic;
    // begin_request_sample calls the sink_stream request_sample;
    // only the current topology triggers a topology switch;
    // returns false if the stream is not found in the topology or the stream returns fatal_error
    bool begin_request_sample(media_stream* sink_stream, const request_packet&, 
        const media_topology_t& topology);
};

typedef std::shared_ptr<media_session> media_session_t;