#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include "request_packet.h"
#include "async_callback.h"
#include "output_file.h"
#include "transform_aac_encoder.h"
#include "assert.h"
#include <vector>
#include <mutex>
#include <chrono>

class sink_audio;
class stream_audio;
typedef std::shared_ptr<sink_audio> sink_audio_t;
typedef std::shared_ptr<stream_audio> stream_audio_t;

class sink_audio : public media_sink
{
    friend class stream_audio;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
public:
    explicit sink_audio(const media_session_t& session);

    void initialize();

    stream_audio_t create_stream(presentation_clock_t&&);
};

class stream_audio : public media_stream_clock_sink
{
    friend class stream_mpeg2;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
	struct empty {};
	typedef request_queue<empty> request_queue;
private:
    sink_audio_t sink;
    volatile bool requesting, processing;
    /*bool running;*/

    media_topology_t topology;
    time_unit stop_point;

    std::atomic_int requests;
    int max_requests;
	request_queue requests_queue;

    // for debug
    int unavailable;
    /*bool ran_once, stopped;*/

    // media_stream_clock_sink
    void on_stream_start(time_unit);
    void on_stream_stop(time_unit);

    void dispatch_request(const request_packet&, bool no_drop = false);
    void dispatch_process();
public:
    explicit stream_audio(const sink_audio_t& sink);

    // media_stream
    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};