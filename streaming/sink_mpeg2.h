#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include "stream_worker.h"
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

#include <mfapi.h>
#include <mfreadwrite.h>
#include <mfidl.h>

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

    stream_mpeg2_t create_stream(presentation_clock_t&&, const stream_audio_t&);
    stream_worker_t create_worker_stream();

    bool is_started() const {return this->started;}
};

class stream_mpeg2 : public media_stream_clock_sink, public presentation_clock_sink
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    /*typedef presentation_time_source::time_unit_t time_unit_t;*/
private:
    sink_mpeg2_t sink;
    volatile bool requesting, processing;
    /*bool running;*/
    bool discontinuity;

    media_topology_t topology;
    time_unit stop_point;

    // TODO: obsolete
    std::recursive_mutex worker_streams_mutex;
    // TODO: obsolete
    stream_worker_t worker_stream;
    stream_audio_t audio_sink_stream;
    time_unit last_due_time;

    std::atomic_int requests;
    int max_requests;

    // for debug
    int unavailable;

    /*DWORD work_queue_id;*/

    // media_stream_clock_sink
    void on_component_start(time_unit);
    void on_component_stop(time_unit);
    void on_stream_start(time_unit);
    void on_stream_stop(time_unit);
    // presentation_clock_sink
    void scheduled_callback(time_unit due_time);

    void schedule_new(time_unit due_time);
    void dispatch_request(const request_packet&, bool no_drop = false);
    void dispatch_process();
public:
    stream_h264_encoder_t encoder_stream;

    stream_mpeg2(const sink_mpeg2_t& sink, const stream_audio_t&);
    ~stream_mpeg2();

    void add_worker_stream(const stream_worker_t& worker_stream);

    // presentation_clock_sink
    bool get_clock(presentation_clock_t&);
    // media_stream
    result_t request_sample(request_packet&, const media_stream* = NULL) {assert_(false); return OK;}
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};