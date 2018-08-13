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

#include <mfapi.h>
#include <mfreadwrite.h>
#include <mfidl.h>

class sink_mpeg2;
class stream_mpeg2;
typedef std::shared_ptr<sink_mpeg2> sink_mpeg2_t;
typedef std::shared_ptr<stream_mpeg2> stream_mpeg2_t;
typedef stream_worker<sink_mpeg2_t> stream_mpeg2_worker;
typedef std::shared_ptr<stream_mpeg2_worker> stream_mpeg2_worker_t;

class sink_mpeg2 : public media_sink
{
    friend class stream_mpeg2;
public:
    struct packet
    {
        output_file_t output;
        media_sample_view_h264 sample;
    };
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<sink_mpeg2> async_callback_t;
    typedef request_queue<packet> request_queue;
    typedef request_queue::request_t request_t;
private:
    HANDLE stopped_signal;
    request_queue write_queue;
    CComPtr<async_callback_t> write_packets_callback;

    std::recursive_mutex topology_switch_mutex;

    media_session_t audio_session;
    media_topology_t pending_audio_topology;

    output_file_t file_output;
    std::mutex writing_mutex;

    void write_packets();
    void write_packets_cb(void*);
public:
    sink_mpeg2(const media_session_t& session, const media_session_t& audio_session);
    ~sink_mpeg2();

    const output_file_t& get_output() const {return this->file_output;}

    // TODO: initialize fps
    void initialize(
        bool null_file,
        HANDLE stopped_signal,
        const CComPtr<IMFMediaType>& video_type, 
        const CComPtr<IMFMediaType>& audio_type);

    // this function makes sure that the both topologies are switched at the same time
    void switch_topologies(
        const media_topology_t& video_topology,
        const media_topology_t& audio_topology);
    void start_topologies(
        time_unit,
        const media_topology_t& video_topology,
        const media_topology_t& audio_topology);

    stream_mpeg2_t create_stream(presentation_clock_t&, const stream_audio_t&);
    stream_mpeg2_worker_t create_worker_stream();
};

class stream_mpeg2 : public media_stream_clock_sink, public presentation_clock_sink
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    sink_mpeg2_t sink;
    bool running;

    std::recursive_mutex worker_streams_mutex;
    std::vector<stream_mpeg2_worker_t> worker_streams;
    stream_audio_t audio_sink_stream;
    output_file_t output;

    // for debug
    int unavailable;

    DWORD work_queue_id;

    // media_stream_clock_sink
    void on_component_start(time_unit);
    void on_component_stop(time_unit);
    void on_stream_start(time_unit);
    void on_stream_stop(time_unit);
    // presentation_clock_sink
    void scheduled_callback(time_unit due_time);

    void schedule_new(time_unit due_time);
    void dispatch_request(request_packet&, bool no_drop = false);
public:
    stream_h264_encoder_t encoder_stream;

    stream_mpeg2(const sink_mpeg2_t& sink, const stream_audio_t&, const output_file_t&);
    ~stream_mpeg2();

    void add_worker_stream(const stream_mpeg2_worker_t& worker_stream);

    // presentation_clock_sink
    bool get_clock(presentation_clock_t& c) {return this->sink->session->get_current_clock(c);}
    // media_stream
    result_t request_sample(request_packet&, const media_stream* = NULL) {assert_(false); return OK;}
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};