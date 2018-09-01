#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include "stream_worker.h"
#include "request_packet.h"
#include "async_callback.h"
#include "output_file.h"
#include "transform_aac_encoder.h"
#include "assert.h"
#include <vector>
#include <mutex>
#include <chrono>

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
    typedef request_queue<media_sample_aac> request_queue;
    typedef std::chrono::duration<time_unit, 
        std::ratio<transform_aac_encoder::input_frames, transform_aac_encoder::sample_rate>>
        periodicity_t;
private:
public:
    explicit sink_audio(const media_session_t& session);

    void initialize();

    stream_audio_t create_stream(presentation_clock_t&);
    // worker streams duplicate the topology so that individual branches can be
    // multithreaded
    stream_audio_worker_t create_worker_stream();
};

class stream_audio : public media_stream_clock_sink
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
    bool ran_once, stopped;

    // media_stream_clock_sink
    void on_stream_start(time_unit);
    void on_stream_stop(time_unit);

    void dispatch_request(request_packet&, bool no_drop = false);
public:
    explicit stream_audio(const sink_audio_t& sink);

    void add_worker_stream(const stream_audio_worker_t& worker_stream);

    // uses a special stream branch so that the request won't be dropped
    result_t request_sample_last(time_unit);

    // media_stream
    result_t request_sample(request_packet&, const media_stream* = NULL);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};