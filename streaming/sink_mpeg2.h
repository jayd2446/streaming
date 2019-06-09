#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include "request_packet.h"
#include "async_callback.h"
#include "sink_audio.h"
#include "media_session.h"
#include "transform_h264_encoder.h"
#include "output_file.h"
#include "assert.h"
#include <vector>
#include <mutex>
#include <chrono>
#include <atomic>

class sink_mpeg2;
class stream_mpeg2;
typedef std::shared_ptr<sink_mpeg2> sink_mpeg2_t;
typedef std::shared_ptr<stream_mpeg2> stream_mpeg2_t;

class sink_mpeg2 : public media_sink
{
    friend class stream_mpeg2;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<sink_mpeg2> async_callback_t;
private:
    std::recursive_mutex topology_switch_mutex;
    bool started;

    media_session_t audio_session;
    media_topology_t pending_audio_topology;
public:
    sink_mpeg2(const media_session_t& session, const media_session_t& audio_session);
    ~sink_mpeg2();

    // TODO: initialize fps
    void initialize();

    // these functions make sure that the both topologies are switched at the same time
    void switch_topologies(
        const media_topology_t& video_topology,
        const media_topology_t& audio_topology);
    void start_topologies(
        time_unit,
        const media_topology_t& video_topology,
        const media_topology_t& audio_topology);

    stream_mpeg2_t create_stream(media_message_generator_t&&, const stream_audio_t&);

    bool is_started() const {return this->started;}
};

class stream_mpeg2 : public media_stream_message_listener, public media_clock_sink
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    /*struct empty {};
    typedef request_queue<empty> request_queue;*/
private:
    sink_mpeg2_t sink;
    bool requesting;
    bool discontinuity;

    media_topology_t topology;
    bool stopping;
    time_unit stop_point;

    stream_audio_t audio_sink_stream;

    // TODO: this probably should be moved to sink_mpeg, so that topology switching doesn't
    // reset the request limit
    std::atomic_int requests;
    int max_requests;
    /*request_queue requests_queue;*/

    // for debug
    int unavailable;

    /*DWORD work_queue_id;*/

    // media_stream_message_listener
    void on_component_start(time_unit);
    void on_component_stop(time_unit);
    void on_stream_start(time_unit);
    void on_stream_stop(time_unit);
    // media_clock_sink
    void scheduled_callback(time_unit due_time);

    void schedule_new(time_unit due_time);
    void dispatch_request(const request_packet&, bool no_drop = false);
public:
    stream_h264_encoder_t encoder_stream;

    stream_mpeg2(const sink_mpeg2_t& sink, const stream_audio_t&);
    ~stream_mpeg2();

    // media_clock_sink
    bool get_clock(media_clock_t&);
    // media_stream
    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};