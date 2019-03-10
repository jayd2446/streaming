#pragma once
#include "media_component.h"
#include "media_stream.h"
#include "media_sample.h"
#include "async_callback.h"
#include "request_packet.h"
#include "request_dispatcher.h"
#include <atlbase.h>
#include <limits>
#include <iostream>
#include <queue>
#include <optional>

#undef min
#undef max

// source specialization base class for components;
// derived classes should only access the payload(sample) field of the request_queue::request_t type
// TODO: enforce this^
// the payload type is wrapped into an optional type to enable null args

template<class SourceBase>
class stream_source_base;

template<class Args>
class source_base : public media_component
{
    friend class stream_source_base<source_base<Args>>;
public:
    typedef async_callback<source_base> async_callback_t;
    typedef stream_source_base<source_base> stream_source_base;
    typedef std::shared_ptr<stream_source_base> stream_source_base_t;
    typedef Args args_t;
    struct payload_t {bool drain; args_t args;};
    typedef request_queue<std::optional<payload_t>> request_queue;
    // TODO: this should not be a typedef
    typedef typename request_queue::request_t request_t;
    typedef request_dispatcher<request_t> request_dispatcher;
private:
    std::pair<frame_unit /*num*/, frame_unit /*den*/> framerate;
    request_queue requests;
    std::queue<request_t> drain_requests;
    std::shared_ptr<request_dispatcher> dispatcher;

    CHandle serve_callback_event;
    CComPtr<async_callback_t> serve_callback;
    CComPtr<IMFAsyncResult> serve_callback_result;
    MFWORKITEM_KEY serve_callback_key;
    bool serve_in_wait_queue;

    bool process_request(request_t&);
    void serve_cb(void*);
protected:
    // derived class must call this
    void initialize(frame_unit frame_rate_num, frame_unit frame_rate_den);

    virtual stream_source_base_t create_derived_stream() = 0;
    // sets the end of samples to 'end',
    // returns whether the end is undefined(=there are no samples available);
    // singlethreaded
    virtual bool get_samples_end(const request_t&, frame_unit& end) = 0;
    // populates the sample field of the request;
    // fetched samples must include padding frames;
    // make_request must add frames up to the frame_end point only;
    // the sample collection in the request must not be empty;
    // singlethreaded;
    // on drain event, the make_request might be called multiple times with the same request arg
    virtual void make_request(request_t&, frame_unit frame_end) = 0;
    // the request_t might contain null args;
    // multithreaded
    virtual void dispatch(request_t&) = 0;
public:
    explicit source_base(const media_session_t& session);
    virtual ~source_base();

    stream_source_base_t create_stream(presentation_clock_t&& clock);
};

template<class SourceBase>
class stream_source_base : public media_stream_clock_sink
{
public:
    typedef SourceBase source_base;
    typedef std::shared_ptr<source_base> source_base_t;
    typedef typename source_base::request_t request_t;
    typedef typename source_base::payload_t payload_t;
private:
    source_base_t source;
    time_unit drain_point;

    virtual void on_stream_stop(time_unit);
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
    serve_in_wait_queue(false),
    serve_callback_event(CreateEvent(NULL, TRUE, FALSE, NULL)),
    dispatcher(new request_dispatcher)
{
    HRESULT hr = S_OK;

    if(!this->serve_callback_event)
        CHECK_HR(hr = E_UNEXPECTED);

    this->serve_callback.Attach(new async_callback_t(&source_base::serve_cb));
    CHECK_HR(hr = MFCreateAsyncResult(NULL, &this->serve_callback->native,
        NULL, &this->serve_callback_result));

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

template<typename T>
source_base<T>::~source_base()
{
    HRESULT hr = S_OK;

    // canceling the work item is important so that the data associated with the callback
    // is released
    if(this->serve_in_wait_queue)
        hr = MFCancelWorkItem(this->serve_callback_key);
}

template<typename T>
void source_base<T>::initialize(frame_unit frame_rate_num, frame_unit frame_rate_den)
{
    HRESULT hr = S_OK;

    this->framerate.first = frame_rate_num;
    this->framerate.second = frame_rate_den;

    CHECK_HR(hr = this->serve_callback->mf_put_waiting_work_item(
        this->shared_from_this<source_base>(), this->serve_callback_event, 0,
        this->serve_callback_result, &this->serve_callback_key));
    this->serve_in_wait_queue = true;

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

template<typename T>
bool source_base<T>::process_request(request_t& request)
{
    const frame_unit request_end = convert_to_frame_unit(
        request.rp.request_time,
        this->framerate.first, this->framerate.second);
    const bool drain = request.sample->drain;

    frame_unit samples_end;
    const bool valid_end = this->get_samples_end(request, samples_end);
    const bool serve_drain_request = (drain && valid_end && samples_end >= request_end);
    const bool serve_request = serve_drain_request || !drain;

    if(serve_drain_request)
        std::cout << "drain on source_base finished" << std::endl;

    if(valid_end)
    {
        const frame_unit end = std::min(request_end, samples_end);
        this->make_request(request, end);
    }
    else if(serve_request)
        // reset the arg to null if the request is served without data
        request.sample.reset();

    return serve_request;
}

template<typename T>
void source_base<T>::serve_cb(void*)
{
    // singlethreaded

    HRESULT hr = S_OK;

    bool run_once = true;
    request_t* request_ptr;
    while((request_ptr = this->requests.get()) || (!this->drain_requests.empty() && run_once))
    {
        if(request_ptr && request_ptr->sample->drain)
        {
            this->drain_requests.push(std::move(*request_ptr));
            this->requests.pop();
            request_ptr = NULL;
        }

        if(this->drain_requests.empty())
        {
            // normal processing

            if(this->process_request(*request_ptr))
            {
                request_t request;
                this->requests.pop(request);

                // dispatch to work queue with the request param
                this->dispatcher->dispatch_request(std::move(request),
                    std::bind(&source_base::dispatch, this->shared_from_this<source_base>(),
                        std::placeholders::_1));
            }
        }
        else
        {
            // drain processing

            if(request_ptr)
            {
                // serve all non drain requests with null args;
                // currently it is assumed that none of the downstream components do any
                // processing on null args
                request_ptr->sample.reset();
                this->dispatch(*request_ptr);
                this->requests.pop();
            }

            request_ptr = &this->drain_requests.front();
            if(this->process_request(*request_ptr))
            {
                request_t request = std::move(*request_ptr);
                this->drain_requests.pop();

                // dispatch to work queue with the request param
                this->dispatcher->dispatch_request(std::move(request),
                    std::bind(&source_base::dispatch, this->shared_from_this<source_base>(),
                        std::placeholders::_1));
            }
            else
                run_once = false;
        }

        // request_ptr is a dangling pointer at this point
        request_ptr = NULL;
    }

    ResetEvent(this->serve_callback_event);

    CHECK_HR(hr = this->serve_callback->mf_put_waiting_work_item(
        this->shared_from_this<source_base>(), this->serve_callback_event, 0,
        this->serve_callback_result, &this->serve_callback_key));

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
}

template<typename T>
typename source_base<T>::stream_source_base_t 
source_base<T>::create_stream(presentation_clock_t&& clock)
{
    stream_source_base_t stream = this->create_derived_stream();
    stream->register_sink(clock);
    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<typename T>
stream_source_base<T>::stream_source_base(const source_base_t& source) :
    media_stream_clock_sink(source.get(), SOURCE),
    source(source),
    drain_point(std::numeric_limits<time_unit>::min())
{
}

template<typename T>
void stream_source_base<T>::on_stream_stop(time_unit t)
{
    this->drain_point = t;
}

template<typename T>
media_stream::result_t stream_source_base<T>::request_sample(
    const request_packet& rp, const media_stream*)
{
    this->source->requests.initialize_queue(rp);

    request_t request;
    request.rp = rp;
    request.stream = this;
    request.sample = std::make_optional<payload_t>();
    request.sample->drain = (this->drain_point == rp.request_time);
    this->source->requests.push(request);

    return OK;
}

template<typename T>
media_stream::result_t stream_source_base<T>::process_sample(
    const media_component_args*, const request_packet&, const media_stream*)
{
    SetEvent(this->source->serve_callback_event);
    return OK;
}

#undef CHECK_HR