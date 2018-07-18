#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include "stream_worker.h"
#include "request_packet.h"
#include "async_callback.h"
#include "source_loopback.h"
#include "output_file.h"
#include "assert.h"
#include <vector>
#include <mutex>

#include <mfapi.h>
#include <mfreadwrite.h>
#include <mfidl.h>

class sink_audio;
class stream_audio;
typedef std::shared_ptr<sink_audio> sink_audio_t;
typedef std::shared_ptr<stream_audio> stream_audio_t;
typedef stream_worker<sink_audio_t> stream_audio_worker;
typedef std::shared_ptr<stream_audio_worker> stream_audio_worker_t;

class sink_audio : public media_sink
{
    friend class stream_audio;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<sink_audio> async_callback_t;
    typedef request_queue<media_sample_aac> request_queue;
    typedef request_queue::request_t request_t;
private:
    request_queue write_queue;
    output_file_t file_output;
    CComPtr<async_callback_t> write_packets_callback;
    std::recursive_mutex writing_mutex;

    void write_packets();
    void write_packets_cb(void*);
public:
    explicit sink_audio(const media_session_t& session);

    void initialize(const output_file_t& file_output);

    stream_audio_t create_stream(presentation_clock_t&);
    // worker streams duplicate the topology so that individual branches can be
    // multithreaded
    stream_audio_worker_t create_worker_stream();
};

class stream_audio : public media_stream, public presentation_clock_sink
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    sink_audio_t sink;
    bool running;

    std::recursive_mutex worker_streams_mutex;
    std::vector<stream_audio_worker_t> worker_streams;

    // for debug
    int unavailable;

    // presentation_clock_sink
    bool on_clock_start(time_unit);
    void on_clock_stop(time_unit);
    void scheduled_callback(time_unit) {assert_(false);}

    void dispatch_request(request_packet&);
public:
    explicit stream_audio(const sink_audio_t& sink);

    void add_worker_stream(const stream_audio_worker_t& worker_stream);

    bool get_clock(presentation_clock_t& c) {return this->sink->session->get_current_clock(c);}
    result_t request_sample(request_packet&, const media_stream* = NULL);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};