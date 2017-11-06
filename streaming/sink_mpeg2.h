#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include "stream_worker.h"
#include "request_packet.h"
#include "async_callback.h"
#include "sink_audio.h"
#include "media_session.h"
#include "transform_h264_encoder.h"
#include <vector>
#include <mutex>
#include "assert.h"

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
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<sink_mpeg2> async_callback_t;
    typedef request_queue::request_t request_t;
private:
    request_queue write_queue;
    CComPtr<async_callback_t> write_packets_callback;

    stream_audio_t audio_sink_stream, new_audio_sink_stream;
    media_topology_t new_audio_topology;

    CComPtr<IMFMediaSink> mpeg_media_sink;
    CComPtr<IMFSinkWriter> writer;
    CComPtr<IMFByteStream> byte_stream;
    CComPtr<IMFMediaType> video_type, audio_type;
    std::recursive_mutex writing_mutex;

    void write_packets();
    void write_packets_cb(void*);
public:
    media_session_t audio_session;

    explicit sink_mpeg2(const media_session_t& session);
    ~sink_mpeg2();

    const CComPtr<IMFSinkWriter>& get_writer() const {return this->writer;}

    void initialize(const CComPtr<IMFMediaType>& video_type, const CComPtr<IMFMediaType>& audio_type);
    void set_new_audio_topology(
        const stream_audio_t& audio_sink_stream,
        const media_topology_t& audio_topology);

    stream_mpeg2_t create_stream(presentation_clock_t&);
    stream_mpeg2_worker_t create_worker_stream();
};

class stream_mpeg2 : public media_stream, public presentation_clock_sink
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    sink_mpeg2_t sink;
    bool running;

    std::recursive_mutex worker_streams_mutex;
    std::vector<stream_mpeg2_worker_t> worker_streams;

    // for debug
    int unavailable;

    DWORD work_queue_id;

    // presentation_clock_sink
    bool on_clock_start(time_unit);
    void on_clock_stop(time_unit);
    void scheduled_callback(time_unit due_time);

    void schedule_new(time_unit due_time);
    void dispatch_request(request_packet&);
public:
    stream_h264_encoder_t encoder_stream;

    explicit stream_mpeg2(const sink_mpeg2_t& sink);
    ~stream_mpeg2();

    void add_worker_stream(const stream_mpeg2_worker_t& worker_stream);

    bool get_clock(presentation_clock_t& c) {return this->sink->session->get_current_clock(c);}
    result_t request_sample(request_packet&, const media_stream* = NULL) {assert_(false); return OK;}
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};