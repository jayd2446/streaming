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

// orchestrates the data flow between components in the media pipeline;
// translates the source's media time to the presentation time

/*

on topology change when the original sink sends a request, media session will
send a notifypreroll event to the new sink, and the new sink will request a sample aswell.

mixer transform must wait for all the samples to arrive before mixing. the earliest
time stamp will be the output time stamp.
// (live media sources will generate samples asynchronously based on framerate and update their buffers)

when the media session is ready to deliver the prerolled sample to the new sink, it will
stop the current topology's media sink by stop() and start the new topology with the
old topology's time.(the stop can happen instantly when the media session gets a request)
the new sink either schedules the sample for rendering or drops it.
it also sets a periodic sample request based on that timestamp.

presentation time will by default lag by 1 fps behind the time stamps

(media sink will take a standalone encoding transform as reference.)
media sink itself will also render the frame to live preview window

media sink will periodically request new samples based on the
presentation sample time(e.g 1/60 secs after the first time stamp
new request will be made)

*/

class media_stream;

class media_session : public enable_shared_from_this
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    /*volatile bool is_started;*/
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
    /*bool started() const {return this->is_started;}*/
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
    // its own function

    // TODO: subsequent request sample and give calls must not fail;
    // failing might lead to deadlocks in sink

    // TODO: use same return values for media streams and media session

    // request_sample returns false if the stream isn't found on the active topology;
    // request_sample won't fail when the is_sink is false
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

    // breaks the circular dependency between components and the session;
    // the session is considered invalid after calling this;
    // the components bound to this session must be reinitialized aswell
    /*void shutdown();*/
};

typedef std::shared_ptr<media_session> media_session_t;