#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "media_sample.h"
#include "request_packet.h"
#include "assert.h"
#include <utility>
#include <memory>
#include <vector>
#include <limits>
#include <algorithm>
#include <mutex>
#include <iostream>
#include <atomic>

// on stream stop the component should serve requests to the request point, so that
// a component that might stop receives enough samples

// TODO: investigate reserve calls

// TODO: mix shouldn't be called if the first and end are the same
// TODO: derived class shouldn't specify the optional type;
// it should be internal to mixer

template<class TransformMixer> 
class stream_mixer;

// in arg and out arg are assumed to be of std optional type
template<class InArg, class UserParamsController, class OutArg>
class transform_mixer : public media_source
{
    friend class stream_mixer<transform_mixer<InArg, UserParamsController, OutArg>>;
public:
    typedef stream_mixer<transform_mixer<InArg, UserParamsController, OutArg>> stream_mixer;
    typedef std::shared_ptr<stream_mixer> stream_mixer_t;
    typedef InArg in_arg_t;
    typedef OutArg out_arg_t;
    typedef std::shared_ptr<UserParamsController> user_params_controller_t;
    typedef typename UserParamsController::params_t user_params_t;

    // since the packet_t is copied to the leftover buffer, it should be lightweight;
    // packet_t is the augmented args for each input stream
    struct packet_t
    {
        media_stream* input_stream;
        in_arg_t arg;
        bool valid_user_params;
        user_params_t user_params;
        size_t stream_index;
    };
    struct args_t
    {
        std::vector<packet_t> container;
    };
    typedef std::pair<unsigned short /*samples received*/, args_t> request_t;
private:
    bool initialized;
    std::pair<frame_unit /*num*/, frame_unit /*den*/> framerate;
protected:
    // derived class must call this
    void initialize(frame_unit frame_rate_num, frame_unit frame_rate_den);

    virtual stream_mixer_t create_derived_stream() = 0;
public:
    explicit transform_mixer(const media_session_t& session);
    virtual ~transform_mixer() {}

    stream_mixer_t create_stream(presentation_clock_t&& clock);
};


template<class TransformMixer>
class stream_mixer : public media_stream_clock_sink
{
    using media_stream::connect_streams;
public:
    typedef TransformMixer transform_mixer;
    typedef std::shared_ptr<transform_mixer> transform_mixer_t;
    typedef typename transform_mixer::in_arg_t in_arg_t;
    typedef typename transform_mixer::out_arg_t out_arg_t;
    typedef typename transform_mixer::user_params_controller_t user_params_controller_t;
    typedef typename transform_mixer::user_params_t user_params_t;
    typedef typename transform_mixer::packet_t packet_t;
    typedef typename transform_mixer::args_t args_t;
    typedef typename transform_mixer::request_t request_t;

    struct input_stream_props_t
    {
        media_stream* input_stream;
        user_params_controller_t user_params_controller;
    };

    typedef request_queue<request_t> request_queue;
private:
    transform_mixer_t transform;
    std::atomic<time_unit> drain_point;
    std::vector<input_stream_props_t> input_streams_props;
    request_queue requests;

    // frames below cutoff are dismissed
    frame_unit cutoff;
    std::unique_ptr<args_t[]> leftover;

    void on_stream_start(time_unit);
    void on_stream_stop(time_unit);

    // converts by using the frame rate in component
    frame_unit convert_to_frame_unit(time_unit);
    void initialize_packet(packet_t&);
    frame_unit find_common_frame_end(const args_t&);
    void process(typename request_queue::request_t&, bool drain);
protected:
    // moves all frames from the sample in old_in_arg to the sample in in_arg up to end
    // and updates the fields for in_arg and old_in_arg;
    // the sample in old_in_arg is assumed to be non null;
    // returns whether the old_in_arg became null(all frames were moved);
    // discarded flags indicates whether the sample is immediately discarded
    virtual bool move_frames(in_arg_t& in_arg, in_arg_t& old_in_arg, frame_unit end, bool discarded) = 0;
    // mixes all the frames in args to out up to end;
    // NOTE: mixing must be multithreading safe
    virtual void mix(out_arg_t& out, args_t&, frame_unit first, frame_unit end) = 0;
public:
    explicit stream_mixer(const transform_mixer_t& transform);
    virtual ~stream_mixer() {}

    // user_params can be NULL
    void connect_streams(
        const media_stream_t& from,
        const user_params_controller_t& user_params_controller,
        const media_topology_t&);

    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};



template<class T, class U, class V>
transform_mixer<T, U, V>::transform_mixer(const media_session_t& session) :
    media_source(session), initialized(false)
{
}

template<class T, class U, class V>
void transform_mixer<T, U, V>::initialize(frame_unit frame_rate_num, frame_unit frame_rate_den)
{
    this->initialized = true;
    this->framerate.first = frame_rate_num;
    this->framerate.second = frame_rate_den;
}

template<class T, class U, class V>
typename transform_mixer<T, U, V>::stream_mixer_t transform_mixer<T, U, V>::create_stream(
    presentation_clock_t&& clock)
{
    stream_mixer_t stream = this->create_derived_stream();
    stream->register_sink(clock);
    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<class T>
stream_mixer<T>::stream_mixer(const transform_mixer_t& transform) :
    media_stream_clock_sink(transform.get()),
    transform(transform),
    drain_point(std::numeric_limits<time_unit>::min()),
    cutoff(std::numeric_limits<time_unit>::min())
{
}

template<class T>
void stream_mixer<T>::on_stream_start(time_unit t)
{
    assert_(this->transform->initialized);
    this->cutoff = this->convert_to_frame_unit(t);

    // initialize the leftover buffer
    assert_(this->input_streams_props.size() > 0);
    this->leftover.reset(new args_t[this->input_streams_props.size()]);
}

template<class T>
void stream_mixer<T>::on_stream_stop(time_unit t)
{
    this->drain_point = t;
}

template<class T>
frame_unit stream_mixer<T>::convert_to_frame_unit(time_unit t)
{
    assert_(this->transform->initialized);

    return ::convert_to_frame_unit(t, 
        this->transform->framerate.first, this->transform->framerate.second);
}

template<class T>
void stream_mixer<T>::initialize_packet(packet_t& packet)
{
    packet.input_stream = this->input_streams_props[packet.stream_index].input_stream;
    packet.valid_user_params = !!this->input_streams_props[packet.stream_index].user_params_controller;
    if(packet.valid_user_params)
        this->input_streams_props[packet.stream_index].user_params_controller->get_params(
            packet.user_params);
}

template<class T>
frame_unit stream_mixer<T>::find_common_frame_end(const args_t& args)
{
    assert_(args.container.size() == this->input_streams_props.size());

    frame_unit frame_end = std::numeric_limits<frame_unit>::max();
    for(size_t i = 0; i < this->input_streams_props.size(); i++)
    {
        frame_unit leftover_frame_end = std::numeric_limits<frame_unit>::min();
        for(auto&& item : this->leftover[i].container)
        {
            assert_(item.arg);
            leftover_frame_end = std::max(leftover_frame_end, item.arg->frame_end);
        }

        auto&& item = args.container[i];
        if(item.arg)
            frame_end = std::min(frame_end, std::max(item.arg->frame_end, leftover_frame_end));
        else if(!this->leftover[i].container.empty())
            frame_end = std::min(frame_end, leftover_frame_end);
        else
            // common frame end cannot be found if a request is lacking samples
            return std::numeric_limits<frame_unit>::min();
    }

    return frame_end;
}

template<class T>
void stream_mixer<T>::process(typename request_queue::request_t& request, bool drain)
{
    assert_(request.sample.second.container.size() == this->input_streams_props.size());

    args_t& packets = request.sample.second;
    const frame_unit old_cutoff = this->cutoff;
    this->cutoff = std::max(this->find_common_frame_end(packets), old_cutoff);
    if(drain)
    {
        const frame_unit drain_cutoff = this->convert_to_frame_unit(request.rp.request_time);
        // the initialized value of new_cutoff above should be either lower or equal 
        // to drain point, since the sources are allowed to serve samples up to the 
        // request point only;
        // for no gaps in the stream, the drain cutoff should be the same as new_cutoff
        assert_(drain_cutoff >= this->cutoff);
        if(drain_cutoff > this->cutoff)
            std::cout << "warning: a source didn't serve samples up to the drain point" << std::endl;
        this->cutoff = drain_cutoff;
        std::cout << "drain on mixer" << std::endl;
    }

    // move the leftover packets to the request
    for(size_t i = 0; i < this->input_streams_props.size(); i++)
    {
        std::move(this->leftover[i].container.begin(),
            this->leftover[i].container.end(),
            std::back_inserter(packets.container));
        this->leftover[i].container.clear();
    }

    // discard frames that are below the old cutoff point
    for(auto&& item : packets.container)
    {
        in_arg_t discarded;
        if(item.arg)
        {
            const bool ret = 
                this->move_frames(discarded, item.arg, old_cutoff, true);
            /*assert_((ret && item.sample.is_null()) || (!ret && !item.sample.is_null()));*/
        }
    }

    // add frames to the output request that are between the old and the new cutoff point
    for(auto&& item : packets.container)
    {
        in_arg_t new_arg;

        if(item.arg)
        {
            bool ret;
            if(!(ret = this->move_frames(new_arg, item.arg, this->cutoff, false)))
                // assign the item to the leftover buffer since it was only partly moved
                this->leftover[item.stream_index].container.push_back(item);
            // TODO: re enable the asserts once video mixer properly sets the properties
            // of the input args
            /*assert_((ret && item.sample.is_null()) || (!ret && !item.sample.is_null()));*/
        }

        // assign the processed arg to the request
        item.arg = new_arg;
    }

    out_arg_t out;
    const frame_unit cutoff = this->cutoff;
    assert_(old_cutoff <= this->cutoff);

    this->unlock();
    this->mix(out, packets, old_cutoff, cutoff);

    this->transform->session->give_sample(request.stream, 
        out.has_value() ? &(*out) : NULL, request.rp);
}

template<class T>
void stream_mixer<T>::connect_streams(const media_stream_t& from,
    const user_params_controller_t& user_params_controller, const media_topology_t& topology)
{
    input_stream_props_t props;
    props.input_stream = from.get();
    props.user_params_controller = user_params_controller;
    this->input_streams_props.insert(this->input_streams_props.begin(), props);

    media_stream::connect_streams(from, topology);
}

template<class T>
typename stream_mixer<T>::result_t stream_mixer<T>::request_sample(
    const request_packet& rp, const media_stream*)
{
    this->requests.initialize_queue(rp);

    // push the request partially
    {
        typename request_queue::request_t request;
        request.rp = rp;
        request.stream = this;
        this->requests.push(request);
    }
    typename request_queue::request_t* request = this->requests.get(rp.packet_number);
    assert_(request->sample.second.container.empty());

    request->sample.first = 0;
    args_t& packets = request->sample.second;

    // reserve twice the size to accommodate for leftover buffer merging
    packets.container.reserve(this->input_streams_props.size() * 2);
    for(size_t i = 0; i < this->input_streams_props.size(); i++)
    {
        packet_t packet;
        packet.stream_index = i;
        this->initialize_packet(packet);
        packets.container.push_back(packet);
    }

    if(!this->transform->session->request_sample(this, rp))
        return FATAL_ERROR;
    return OK;
}

template<class T>
typename stream_mixer<T>::result_t stream_mixer<T>::process_sample(
    const media_component_args* arg_, const request_packet& rp, const media_stream* prev_stream)
{
    this->lock();

    typename request_queue::request_t* request = this->requests.get(rp.packet_number);
    assert_(request);

    /*Sleep(10);*/

    // find the right packet from the list and assign the sample to it
    bool found = false;
    for(auto&& item : request->sample.second.container)
        if(item.input_stream == prev_stream && !item.arg)
        {
            request->sample.first++;
            if(arg_)
                item.arg = std::make_optional(static_cast<const in_arg_t::value_type&>(*arg_));
            found = true;
            break;
        };
    assert_(found);

    // dispatch all requests that are ready
    for(typename request_queue::request_t* next_request = this->requests.get();
        next_request && 
        (size_t)next_request->sample.first == this->input_streams_props.size();
        next_request = this->requests.get())
    {
        assert_(next_request->stream == this);

        typename request_queue::request_t request;
        this->requests.pop(request);

        this->process(request, (request.rp.request_time == this->drain_point.load()));

        this->lock();
    }

    this->unlock();
    return OK;
}