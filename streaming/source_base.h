#pragma once
#include "media_component.h"
#include "media_stream.h"
#include "media_sample.h"
#include "request_packet.h"
#include "request_dispatcher.h"
#include "request_queue_handler.h"
#include <queue>
#include <optional>
#include <mutex>
#include <atomic>

#undef min
#undef max

// source specialization base class for components;
// the args type is wrapped into an optional type to enable null args

template<class SourceBase>
class stream_source_base;

template<class Args>
class source_base : public media_component
{
    friend class stream_source_base<source_base<Args>>;
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef Args args_t;
    struct payload_t { bool drain; std::optional<args_t> args; };
    typedef stream_source_base<source_base> stream_source_base;
    typedef std::shared_ptr<stream_source_base> stream_source_base_t;
    // TODO: this should not be a typedef
    typedef typename request_queue<payload_t>::request_t request_t;
private:
    std::mutex active_topology_mutex;
    std::queue<media_topology_t> active_topology;
    std::atomic<bool> broken_flag;

    // set_broken must be used instead
    using media_component::request_reinitialization;
protected:
    control_class_t ctrl_pipeline;

    // derived class must call this
    void initialize(const control_class_t& ctrl_pipeline);

    virtual stream_source_base_t create_derived_stream() = 0;
    // sets the end of samples to 'end',
    // returns whether the end is undefined(=there are no samples available);
    // multithreaded
    virtual bool get_samples_end(time_unit request_time, frame_unit& end) = 0;
    // sets the args field in request_t;
    // fetched samples must include padding frames;
    // make_request must add frames up to the frame_end point only;
    // the sample collection in args must not be empty;
    // args can be set to NULL;
    // singlethreaded
    virtual void make_request(request_t&, frame_unit frame_end) = 0;
    // the request_t might contain null args;
    // multithreaded
    virtual void dispatch(request_t&) = 0;

    // sets the component as broken;
    // source_base serves frame skips when the component is broken;
    // NOTE: should not be called in request call chain, which means that
    // this should not be called in get_samples_end
    void set_broken();
public:
    explicit source_base(const media_session_t& session);
    virtual ~source_base();

    bool is_broken() const { return this->broken_flag; }

    stream_source_base_t create_stream(media_message_generator_t&& message_generator);
};

template<class SourceBase>
class stream_source_base : 
    public media_stream_message_listener,
    request_queue_handler<typename SourceBase::payload_t>
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef SourceBase source_base;
    typedef std::shared_ptr<source_base> source_base_t;
    typedef request_dispatcher<typename source_base::request_t> request_dispatcher;
    typedef typename request_queue_handler<typename SourceBase::payload_t>::request_queue 
        request_queue;
private:
    source_base_t source;
    std::weak_ptr<media_topology> this_topology;
    bool drainable_or_drained;
    std::shared_ptr<::request_dispatcher<void*>> serve_dispatcher;
    std::shared_ptr<request_dispatcher> dispatcher;

    // wrapper for source_base::get_samples_end, handles broken functionality
    bool get_samples_end(time_unit request_time, frame_unit& end);

    // media_stream_message_listener
    void on_stream_start(time_unit);
    bool is_drainable_or_drained(time_unit);
    // request_queue_handler
    bool on_serve(typename request_queue::request_t&);
    typename request_queue::request_t* next_request();
public:
    explicit stream_source_base(const source_base_t&);
    virtual ~stream_source_base() {}

    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

template<typename T>
source_base<T>::source_base(const media_session_t& session) :
    media_component(session),
    broken_flag(false)
{
}

template<typename T>
source_base<T>::~source_base()
{
}

template<typename T>
void source_base<T>::set_broken()
{
    this->broken_flag = true;
    this->request_reinitialization(this->ctrl_pipeline);
}

template<typename T>
void source_base<T>::initialize(const control_class_t& ctrl_pipeline)
{
    this->ctrl_pipeline = ctrl_pipeline;
}

template<typename T>
typename source_base<T>::stream_source_base_t
source_base<T>::create_stream(media_message_generator_t&& message_generator)
{
    stream_source_base_t stream = this->create_derived_stream();
    stream->register_listener(message_generator);
    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<typename T>
stream_source_base<T>::stream_source_base(const source_base_t& source) :
    media_stream_message_listener(source.get(), SOURCE),
    source(source),
    drainable_or_drained(false),
    dispatcher(new request_dispatcher),
    serve_dispatcher(new ::request_dispatcher<void*>)
{
}

template<typename T>
bool stream_source_base<T>::get_samples_end(time_unit request_time, frame_unit& end)
{
    const bool broken_flag = this->source->broken_flag;
    if(broken_flag)
    {
        // source_base serves frame skips when the source is broken
        end = convert_to_frame_unit(request_time,
            this->source->session->frame_rate_num,
            this->source->session->frame_rate_den);
        return true;
    }
    else
        return this->source->get_samples_end(request_time, end);
}

template<typename T>
void stream_source_base<T>::on_stream_start(time_unit)
{
    scoped_lock lock(this->source->active_topology_mutex);
    this->source->active_topology.push(this->source->session->get_current_topology());
    this->this_topology = this->source->session->get_current_topology();
}

template<typename T>
bool stream_source_base<T>::is_drainable_or_drained(time_unit t)
{
    // drain can be finished only when the active_topology equals this topology,
    // otherwise there exists a chance where the last_request is dispatched while the
    // active topology is still the previous one
    // TODO: source_base needs a rework

    if(!this->drainable_or_drained)
    {
        media_topology_t active_topology;
        {
            scoped_lock lock(this->source->active_topology_mutex);
            if(!this->source->active_topology.empty())
                active_topology = this->source->active_topology.front();
        }

        assert_(this->this_topology.lock());
        if(active_topology == this->this_topology.lock())
        {
            frame_unit samples_end;
            const frame_unit request_end = convert_to_frame_unit(t,
                this->source->session->frame_rate_num,
                this->source->session->frame_rate_den);
            const bool valid_end = this->get_samples_end(t, samples_end);
            // drain can be finished when the source has samples up to the drain point
            if(valid_end && (samples_end >= request_end))
                this->drainable_or_drained = true;
        }
    }

    return this->drainable_or_drained;
}

template<typename T>
bool stream_source_base<T>::on_serve(typename request_queue::request_t& request)
{
    media_topology_t active_topology;
    {
        scoped_lock lock(this->source->active_topology_mutex);
        if(!this->source->active_topology.empty())
            active_topology = this->source->active_topology.front();
    }

    // TODO: source_base should serve null if a request has been served with valid data
    // up to request_time already;
    // currently, such case would cause an assert failure;
    // such case might happen on drain operation;
    // ^ should be fixed

    // only serve the request with samples if the request originates from the active topology
    if(active_topology == request.rp.topology)
    {
        frame_unit samples_end;
        const frame_unit request_end = convert_to_frame_unit(request.rp.request_time,
            this->source->session->frame_rate_num,
            this->source->session->frame_rate_den);
        const bool valid_end = this->get_samples_end(request.rp.request_time, samples_end);

        /*assert_(!request.sample->drain || (request.sample->drain && valid_end));*/

        if(valid_end)
        {
            const frame_unit end = std::min(request_end, samples_end);
            // make_request is allowed to set the args in request to null
            this->source->make_request(request, end);
        }
        else
            assert_(!request.sample.args);

        // pop the current active topology if the request has drain flag;
        // it is important to pop the topology after make_request call so that
        // it stays singlethreaded
        if(request.sample.drain)
        {
            scoped_lock lock(this->source->active_topology_mutex);
            assert_(!this->source->active_topology.empty());
            this->source->active_topology.pop();
        }
    }
    else
        assert_(!request.sample.args);

    this->dispatcher->dispatch_request(std::move(request),
        std::bind(&source_base::dispatch, this->source, std::placeholders::_1));

    return true;
}

template<typename T>
typename stream_source_base<T>::request_queue::request_t* stream_source_base<T>::next_request()
{
    return this->requests.get();
}

template<typename T>
media_stream::result_t stream_source_base<T>::request_sample(
    const request_packet& rp, const media_stream*)
{
    this->requests.initialize_queue(rp);

    typename request_queue::request_t request;
    request.rp = rp; 
    request.stream = this;
    request.sample.drain = this->drainable_or_drained;
    this->requests.push(request);

    // sources flip the direction
    this->process_sample(NULL, rp, this);

    return OK;
}

template<typename T>
media_stream::result_t stream_source_base<T>::process_sample(
    const media_component_args*, const request_packet&, const media_stream*)
{
    this->serve_dispatcher->dispatch_request(NULL,
        [this_ = this->shared_from_this<stream_source_base>()](void*) {this_->serve();});

    return OK;
}

#undef CHECK_HR