#pragma once
#include "async_callback.h"
#include "media_sink.h"
#include "media_stream.h"
#include "presentation_clock.h"
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <queue>
#include <utility>
#include <atlbase.h>

/*

mpeg sink implements multithreading with a topology branch granularity;
the host stream is multithreaded, the worker streams are singlethreaded

each topology branch is a separate instance of the same elementary topology

*/

class stream_mpeg_host;
class stream_mpeg;
typedef std::shared_ptr<stream_mpeg_host> stream_mpeg_host_t;
typedef std::shared_ptr<stream_mpeg> stream_mpeg_t;

class sink_mpeg : public media_sink
{
    friend class stream_mpeg_host;
public:
    struct request_t
    {
        time_unit request_time, timestamp;
        int packet_number;
    };
private:
    std::recursive_mutex requests_mutex;
    std::queue<request_t> requests;
    std::atomic_int32_t packet_number;
public:
    explicit sink_mpeg(const media_session_t& session);

    // the host stream will dispatch the request to streams
    // directly without media session's involvement
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

    result_t request_sample(request_packet&);
    result_t process_sample(const media_sample_view_t&, request_packet&);
};

class stream_mpeg_host : public media_stream, public presentation_clock_sink
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<stream_mpeg_host> async_callback_t;
private:
    sink_mpeg_t sink;
    bool running;

    std::recursive_mutex worker_streams_mutex;
    std::vector<stream_mpeg_t> worker_streams;
    CComPtr<async_callback_t> request_callback;

    // presentation_clock_sink
    bool on_clock_start(time_unit, int packet_number);
    void on_clock_stop(time_unit);
    void scheduled_callback(time_unit due_time);

    void schedule_new(time_unit due_time);
    void push_request(time_unit);

    // callback
    void request_cb(void*);
public:
    explicit stream_mpeg_host(const sink_mpeg_t& sink);

    void add_worker_stream(const stream_mpeg_t& worker_stream);

    bool get_clock(presentation_clock_t& c) {return this->sink->session->get_current_clock(c);}
    result_t request_sample(request_packet&);
    result_t process_sample(const media_sample_view_t&, request_packet&);
};