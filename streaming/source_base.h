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

#undef min
#undef max

// source specialization base class for components

template<class SourceBase>
class stream_source_base;

template<class Sample>
class source_base : public media_component
{
    friend class stream_source_base<source_base<Sample>>;
public:
    typedef async_callback<source_base> async_callback_t;
    typedef stream_source_base<source_base> stream_source_base;
    typedef std::shared_ptr<stream_source_base> stream_source_base_t;
    struct sample_t : Sample {bool drain;};
    typedef request_queue<sample_t> request_queue;
    // TODO: this should not be a typedef
    typedef typename request_queue::request_t request_t;
    typedef request_dispatcher<request_t> request_dispatcher;
private:
    std::pair<frame_unit /*num*/, frame_unit /*den*/> framerate;
    request_queue requests;
    std::shared_ptr<request_dispatcher> dispatcher;

    CHandle serve_callback_event;
    CComPtr<async_callback_t> serve_callback;
    CComPtr<IMFAsyncResult> serve_callback_result;
    MFWORKITEM_KEY serve_callback_key;
    bool serve_in_wait_queue;

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
    // make_request should only add frames up to the frame_end point;
    // the sample collection in the request must not be empty;
    // singlethreaded
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
void source_base<T>::serve_cb(void*)
{
    HRESULT hr = S_OK;

    const request_t* readonly_request;
    while(readonly_request = this->requests.get())
    {
        const frame_unit frame_end = convert_to_frame_unit(
            readonly_request->rp.request_time,
            this->framerate.first, this->framerate.second);

        const bool drain = readonly_request->sample.drain;

        // do not serve the request if there's not enough data
        frame_unit samples_end;
        const bool valid_end = this->get_samples_end(*readonly_request, samples_end);
        if(drain && (!valid_end || samples_end < frame_end))
            break;

        if(drain)
            std::cout << "drain on source_base" << std::endl;

        readonly_request = NULL;

        request_t request;
        this->requests.pop(request);
        if(valid_end || drain)
        {
            // there must be a valid end on drain
            assert_(!drain || (drain && valid_end));
            const frame_unit end = drain ? frame_end : std::min(frame_end, samples_end);
            this->make_request(request, end);
        }

        // dispatch to work queue with the request param
        this->dispatcher->dispatch_request(std::move(request), 
            std::bind(&source_base::dispatch, this->shared_from_this<source_base>(), 
                std::placeholders::_1));
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
    // TODO: there exists a chance where when changing a topology while preserving
    // old sources and replacing other components, a request might be dispatched
    // to a component before its request_sample function has been called

    // TODO: remove begin_give_sample and ensure in the request call chain that every request_sample
    // has been called before calling source's request_sample;
    // separate source components from other components and call source components'
    // request sample after completing the request chain for other components

    this->source->requests.initialize_queue(rp);

    request_t request;
    request.rp = rp;
    request.stream = this;
    request.sample.drain = (this->drain_point == rp.request_time);
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