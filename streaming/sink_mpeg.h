#pragma once
#include "async_callback.h"
#include "media_sink.h"
#include "media_stream.h"
#include "presentation_clock.h"
#include "transform_h264_encoder.h"
#include "transform_aac_encoder.h"
#include "source_loopback.h"
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <queue>
#include <map>
#include <utility>
#include <atlbase.h>

#include <mfapi.h>
#include <mfreadwrite.h>
#include <mfidl.h>

/*

mpeg sink implements multithreading with a topology branch granularity;
the host stream is multithreaded, the worker streams are singlethreaded

each topology branch is a separate instance of the same elementary topology

*/

class stream_mpeg_host;
class stream_mpeg;
typedef std::shared_ptr<stream_mpeg_host> stream_mpeg_host_t;
typedef std::shared_ptr<stream_mpeg> stream_mpeg_t;

// TODO: the mpeg packet writing queue might overload
// TODO: the mpeg file writing should be separated
// (can be choosed between rtmp and file)

// in windows 8 the mpeg media sink automatically tries to find the sequence header
// from the input samples
// https://stackoverflow.com/questions/22057696/creating-an-mp4-container-from-an-h-264-byte-stream-annex-b-using-media-found

// TODO: the mpeg sink should take input type parameters from the encoder

class sink_mpeg : public media_sink
{
    friend class stream_mpeg_host;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<sink_mpeg> async_callback_t;
    // used for writing the h264 packet to disk
    struct packet
    {
        request_packet rp;
        media_sample_view_t sample_view;
    };
private:
    // used for writing the packet to disk
    std::recursive_mutex packets_mutex;
    std::map<time_unit /*request time*/, packet> packets;
    std::recursive_mutex processed_packets_mutex;
    std::queue<packet> processed_packets;
    CComPtr<async_callback_t> processing_callback;
    std::recursive_mutex processing_mutex;

    CComPtr<IMFMediaSink> mpeg_media_sink;
    CComPtr<IMFSinkWriter> sink_writer;
    CComPtr<IMFByteStream> byte_stream;
    CComPtr<IMFMediaType> mpeg_file_type, mpeg_file_type_audio;
    std::recursive_mutex writing_mutex;

    media_session_t audio_session;
    source_loopback_t loopback_source;
    transform_aac_encoder_t aac_encoder_transform;
    std::shared_ptr<sink_mpeg> mpeg_sink;

    void new_packet();
    void processing_cb(void*);

    bool parent;

    sink_mpeg(const media_session_t& session, const CComPtr<IMFSinkWriter>&);
public:
    sink_mpeg(const media_session_t& session);
    ~sink_mpeg();

    void initialize(const CComPtr<IMFMediaType>& input_type);
    // the host stream will dispatch the request to streams
    // directly without media session's involvement;
    stream_mpeg_host_t create_host_stream(presentation_clock_t&);
    // streams must be connected to the host stream
    stream_mpeg_t create_worker_stream();
};

typedef std::shared_ptr<sink_mpeg> sink_mpeg_t;

class stream_mpeg : public media_stream
{
    friend class stream_mpeg_host;
private:
    sink_mpeg_t sink;
    volatile bool available;
public:
    explicit stream_mpeg(const sink_mpeg_t& sink);

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};

class stream_mpeg_host : public media_stream, public presentation_clock_sink
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<stream_mpeg_host> async_callback_t;
    // used for dispatching requests to topology branches
    struct request_t
    {
        time_unit request_time, timestamp;
        int packet_number;
    };
private:
    sink_mpeg_t sink;
    bool running;

    std::recursive_mutex worker_streams_mutex;
    std::vector<stream_mpeg_t> worker_streams;

    std::atomic_int32_t packet_number;
    std::atomic<time_unit> due_time;
    stream_h264_encoder_t encoder_stream;
    stream_aac_encoder_t encoder_aac_stream;

    int unavailable;

    // the audio topology is switched to the session
    // when the video topology starts
    media_topology_t audio_topology;

    void set_audio_session(time_unit time_point);

    // presentation_clock_sink
    bool on_clock_start(time_unit);
    void on_clock_stop(time_unit);
    void scheduled_callback(time_unit due_time);

    void schedule_new(time_unit due_time);
    void dispatch_request(time_unit request_time);
public:
    explicit stream_mpeg_host(const sink_mpeg_t& sink);

    void add_worker_stream(const stream_mpeg_t& worker_stream);
    void set_encoder_stream(const stream_h264_encoder_t& e) {this->encoder_stream = e;}
    void set_encoder_stream(const stream_aac_encoder_t& e) {this->encoder_aac_stream = e;}

    bool get_clock(presentation_clock_t& c) {return this->sink->session->get_current_clock(c);}
    result_t request_sample(request_packet&, const media_stream* = NULL);
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};