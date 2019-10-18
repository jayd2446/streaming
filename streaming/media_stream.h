#pragma once
#include "media_sample.h"
#include "enable_shared_from_this.h"
#include <memory>
#include <condition_variable>

class media_topology;
class media_stream;
class media_component;
class media_message_generator;
typedef std::shared_ptr<media_stream> media_stream_t;
typedef std::shared_ptr<media_topology> media_topology_t;
typedef std::shared_ptr<media_message_generator> media_message_generator_t;
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
    // streams from source components must be of source type
    enum stream_t
    {
        OTHER,
        SOURCE
    };
private:
    const stream_t stream_type;
    volatile bool locked;
    std::mutex mutex;
    std::condition_variable cv;
protected:
    // requesting stage shouldn't lock because it can cause a deadlock with the topology
    // switch mutex
    void lock();
    void unlock();
    bool is_locked() const {return this->locked;}
public:
    explicit media_stream(stream_t = OTHER);
    virtual ~media_stream() {}

    bool is_source_stream() const {return (this->stream_type == SOURCE);}

    // throws on error
    virtual void connect_streams(const media_stream_t& from, const media_topology_t&);

    // TODO: remove previous_stream for request_sample

    // requests samples from media session or processes
    // samples if there are any;
    // implements input stream functionality;
    // NOTE: request_sample shouldn't lock anything, because otherwise a deadlock might occur;
    // source's request sample must dispatch to a work queue, because video_sink
    // holds a topology switch mutex, which can cause a deadlock with pipeline mutex;
    virtual result_t request_sample(const request_packet&, const media_stream* previous_stream) = 0;
    // processes the new sample and optionally calls media_session::give_sample;
    // implements output stream functionality;
    // the passed rp(and media session which initiates the call) 
    // stores a reference to this stream, which guarantees that this stream
    // won't be deleted prematurely
    virtual result_t process_sample(
        const media_component_args*, const request_packet&, const media_stream* previous_stream) = 0;
};

// declares overridables for topology messages
class media_stream_message_listener : public media_stream
{
    friend class media_message_generator;
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
    // TODO: this message could be moved to media_source_stream only
    // called by media session to resolve the status of topology draining after on_stream_stop event
    virtual bool is_drainable_or_drained(time_unit) {return true;}
public:
    media_stream_message_listener(const media_component*, stream_t = OTHER);
    virtual ~media_stream_message_listener() {}

    // adds this listener to the list of listeners in the media event generator
    void register_listener(const media_message_generator_t&);
};

typedef std::shared_ptr<media_stream_message_listener> media_stream_message_listener_t;