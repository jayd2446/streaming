#pragma once
#include "media_sample.h"
#include "enable_shared_from_this.h"
#include <memory>
#include <condition_variable>

class media_topology;
class media_stream;
class media_component;
class presentation_clock;
typedef std::shared_ptr<media_stream> media_stream_t;
typedef std::shared_ptr<media_topology> media_topology_t;
typedef std::shared_ptr<presentation_clock> presentation_clock_t;
struct request_packet;

class media_stream : public enable_shared_from_this
{
public:
    typedef std::unique_lock<std::mutex> scoped_lock;
    enum result_t
    {
        OK,
        // the topology encountered an unrecoverable error
        FATAL_ERROR
    };
    // stream type is used to enable multiple outputs from a node while
    // maintaining single request guarantee in the topology
    enum stream_t
    {
        PROCESS_REQUEST,
        PROCESS
    };
private:
    const stream_t stream_type;

    volatile bool locked;
    std::mutex mutex;
    std::condition_variable cv;
protected:
    // requesting stage doesn't need locking unless it modifies fields that are accessed in the
    // processing stage
    void lock();
    void unlock();
    bool is_locked() const {return this->locked;}
public:
    explicit media_stream(stream_t = PROCESS_REQUEST);
    virtual ~media_stream() {}

    stream_t get_stream_type() const {return this->stream_type;}

    // throws on error
    virtual void connect_streams(const media_stream_t& from, const media_topology_t&);

    // requests samples from media session or processes
    // samples if there are any;
    // implements input stream functionality;
    // NOTE: request_sample shouldn't lock anything, because otherwise a deadlock might occur;
    // source's request sample must dispatch to a work queue, because mpeg_sink
    // holds a topology switch mutex, which can cause a deadlock with pipeline mutex;
    virtual result_t request_sample(request_packet&, const media_stream* previous_stream) = 0;
    // processes the new sample and optionally calls media_session::give_sample;
    // implements output stream functionality;
    // the passed rp(and media session which initiates the call) 
    // stores a reference to this stream, which guarantees that this stream
    // won't be deleted prematurely
    virtual result_t process_sample(
        const media_sample&, request_packet&, const media_stream* previous_stream) = 0;
};

// implements event firing
class media_stream_clock_sink : public media_stream
{
    friend class presentation_clock;
private:
    const media_component* component;
    bool unregistered;
protected:
    // called when the component is started/stopped
    virtual void on_component_start(time_unit) {}
    virtual void on_component_stop(time_unit) {}
    // called when the stream is started/stopped
    virtual void on_stream_start(time_unit) {}
    virtual void on_stream_stop(time_unit) {}
public:
    explicit media_stream_clock_sink(const media_component*);
    virtual ~media_stream_clock_sink() {}

    // adds this sink to the list of sinks in the presentation clock
    void register_sink(presentation_clock_t&);
};

typedef std::shared_ptr<media_stream_clock_sink> media_stream_clock_sink_t;