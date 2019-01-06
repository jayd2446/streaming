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

/*

on stream stop event every source should serve samples up to the request time point
(or not);
on component stop the source should serve requests up to the stop point

requests are processed and served in chronological order

derived class is given a range of frame units and it will handle the actual mixing


left over buffer is the size of input streams

*/

template<class TransformMixer> 
class stream_mixer;

template<class Sample, class UserParamsController, class OutSample>
class transform_mixer : public media_source
{
    friend class stream_mixer<transform_mixer<Sample, UserParamsController, OutSample>>;
public:
    typedef stream_mixer<transform_mixer<Sample, UserParamsController, OutSample>> stream_mixer;
    typedef std::shared_ptr<stream_mixer> stream_mixer_t;
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef Sample sample_t;
    typedef OutSample out_sample_t;
    typedef std::shared_ptr<UserParamsController> user_params_controller_t;
    typedef typename UserParamsController::params_t user_params_t;

    struct packet_t
    {
        media_stream* input_stream;
        sample_t sample;
        bool valid_user_params;
        user_params_t user_params;
    };
    struct request_t
    {
        bool drain;
        time_unit drain_point;
        unsigned short samples_received;
        std::vector<packet_t> container;
    };
    //typedef std::pair<size_t /*samples received*/, std::vector<packet_t>> request_t;
private:
    bool initialized;
    std::pair<frame_unit /*num*/, frame_unit /*den*/> framerate;

    // frames below cutoff are dismissed
    frame_unit cutoff;
    request_t leftover;
    std::mutex mutex;
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
    typedef typename transform_mixer::sample_t sample_t;
    typedef typename transform_mixer::out_sample_t out_sample_t;
    typedef typename transform_mixer::user_params_controller_t user_params_controller_t;
    typedef typename transform_mixer::user_params_t user_params_t;
    typedef typename transform_mixer::packet_t packet_t;
    typedef typename transform_mixer::request_t request_t;

    struct input_stream_props_t
    {
        media_stream* input_stream;
        user_params_controller_t user_params_controller;
    };
    typedef request_queue<request_t> request_queue;
private:
    transform_mixer_t transform;
    time_unit drain_point;
    std::vector<input_stream_props_t> input_streams_props;
    request_queue requests;

    void on_component_start(time_unit);
    void on_component_stop(time_unit);
    /*void on_stream_start(time_unit);
    void on_stream_stop(time_unit);*/

    // TODO: on drain(component stop), the cutoff should be at the component stop point

    // converts by using the frame rate in component
    frame_unit convert_to_frame_unit(time_unit);
    void initialize_packet(packet_t&, size_t input_stream_index);
    frame_unit find_common_frame_end(const request_t&);
    void process(typename request_queue::request_t&);
protected:
    // moves all frames from the old_sample buffer to the sample buffer up to end(exclusive)
    // and updates the fields for sample&old_sample;
    // the old_sample is assumed to be non null;
    // returns whether the old_sample became null(all frames were moved)
    virtual bool move_frames(sample_t& sample, sample_t& old_sample, frame_unit end) = 0;
    // mixes all the frames in the request;
    // end is not mixed;
    // mix function needs to sort the request_t list
    virtual void mix(out_sample_t& sample, request_t&, frame_unit first, frame_unit end) = 0;
public:
    explicit stream_mixer(const transform_mixer_t& transform);
    virtual ~stream_mixer() {}

    // TODO: the controller will have an index that tells the z ordering
    // user_params can be NULL
    void connect_streams(
        const media_stream_t& from,
        const user_params_controller_t& user_params_controller,
        const media_topology_t&);

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
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
    drain_point(std::numeric_limits<time_unit>::min())
{
}

template<class T>
void stream_mixer<T>::on_component_start(time_unit t)
{
    assert_(this->transform->initialized);
    this->transform->cutoff = this->convert_to_frame_unit(t);
}

template<class T>
void stream_mixer<T>::on_component_stop(time_unit t)
{
    this->drain_point = t;
}

//template<class T, class U>
//void stream_mixer<T, U>::on_stream_start(time_unit t)
//{
//    // initialize the leftover buffer
//    this->leftover.first = this->input_streams_props.size();
//    this->leftover.second.reset(new packet_t[this->input_streams_props.size()]);
//    for(size_t i = 0; i < this->input_streams_props.size(); i++)
//        this->initialize_packet(this->leftover.second[i], i);
//
//    // allocate the output buffer
//    this->output.first = this->input_streams_props.size();
//    this->output.second.reset(new packet_t[this->input_streams_props.size()]);
//
//    // set the first cutoff point
//    assert_(this->transform->initialized);
//    this->cutoff = this->transform->last_stream_stop;
//}

//template<class T, class U>
//void stream_mixer<T, U>::on_stream_stop(time_unit t)
//{
//    // set the last stream pos for component
//    assert_(component);
//    assert_(this->transform->initialized);
//
//    this->transform->last_stream_stop = this->convert_to_frame_unit(t);
//}

template<class T>
frame_unit stream_mixer<T>::convert_to_frame_unit(time_unit t)
{
    assert_(this->transform->initialized);

    return ::convert_to_frame_unit(t, 
        this->transform->framerate.first, this->transform->framerate.second);
}

template<class T>
void stream_mixer<T>::initialize_packet(packet_t& packet, size_t input_stream_index)
{
    assert_(input_stream_index < this->input_streams_props.size());

    packet.input_stream = this->input_streams_props[input_stream_index].input_stream;
    packet.valid_user_params = !!this->input_streams_props[input_stream_index].user_params_controller;
    if(packet.valid_user_params)
        this->input_streams_props[input_stream_index].user_params_controller->get_params(
            packet.user_params);
}

template<class T>
frame_unit stream_mixer<T>::find_common_frame_end(const request_t& request)
{
    assert_(!request.container.empty());

    frame_unit frame_end = std::numeric_limits<frame_unit>::max();
    for(auto&& item : request.container)
    {
        if(!item.sample.is_null())
            frame_end = std::min(frame_end, item.sample.frame_end);
        else
            // common frame end cannot be found if a request is lacking samples
            return std::numeric_limits<frame_unit>::min();
    }

    return frame_end;
}

template<class T>
void stream_mixer<T>::process(typename request_queue::request_t& request)
{
    assert_(!request.sample.container.empty());

    request_t& packets = request.sample;
    frame_unit old_cutoff, new_cutoff;

    {
        typename transform_mixer::scoped_lock lock(this->transform->mutex);

        // audio mixer shouldn't set the cutoff point on stream stop; only on component stop

        // add the left over buffer to the request
        std::move(this->transform->leftover.container.begin(),
            this->transform->leftover.container.end(),
            std::back_inserter(packets.container));
        this->transform->leftover.container.clear();
        this->transform->leftover.container.reserve(packets.container.size());

        // discard frames that are below the old cutoff point
        for(auto&& item : packets.container)
        {
            // TODO: add print for all discarded frames
            sample_t discarded_sample;
            if(!item.sample.is_null())
            {
                const bool ret = 
                    this->move_frames(discarded_sample, item.sample, this->transform->cutoff);
                assert_((ret && item.sample.is_null()) || (!ret && !item.sample.is_null()));
            }
        }

        new_cutoff = std::max(this->find_common_frame_end(packets), this->transform->cutoff);
        if(packets.drain)
        {
            // the initialized value of new_cutoff above should be either lower or equal 
            // to drain point, since the sources are allowed to serve samples up to the 
            // request point only
            new_cutoff = std::max(new_cutoff, this->convert_to_frame_unit(packets.drain_point));
            std::cout << "drain on mixer" << std::endl;
        }

        // add frames to the output request that are between the old and the new cutoff point
        for(auto&& item : packets.container)
        {
            sample_t new_sample;
            if(!item.sample.is_null())
            {
                bool ret;
                if(!(ret = this->move_frames(new_sample, item.sample, new_cutoff)))
                    // assign the rest of the old sample to the leftover buffer
                    this->transform->leftover.container.push_back(item);
                assert_((ret && item.sample.is_null()) || (!ret && !item.sample.is_null()));
            }

            // assign the new sample to the request
            item.sample = new_sample;
        }

        old_cutoff = this->transform->cutoff;
        this->transform->cutoff = new_cutoff;
    }

    out_sample_t out_sample;
    assert_(old_cutoff <= new_cutoff);
    this->mix(out_sample, packets, old_cutoff, new_cutoff);

    this->unlock();
    this->transform->session->give_sample(request.stream, out_sample, request.rp, false);
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
    request_packet& rp, const media_stream*)
{
    this->requests.initialize_queue(rp);

    // build the request partially
    request_t packets;
    packets.samples_received = 0;
    packets.drain = (this->drain_point == rp.request_time);
    packets.drain_point = this->drain_point;
    // reserve twice the size to accommodate for leftover buffer merging
    packets.container.reserve(this->input_streams_props.size() * 2);
    for(size_t i = 0; i < this->input_streams_props.size(); i++)
    {
        packet_t packet;
        this->initialize_packet(packet, i);
        packets.container.push_back(std::move(packet));
    }

    // add the new request to the queue
    typename request_queue::request_t request;
    request.rp = rp;
    request.stream = this;
    request.sample = packets;
    this->requests.push(request);

    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}

template<class T>
typename stream_mixer<T>::result_t stream_mixer<T>::process_sample(
    const media_sample& sample_, request_packet& rp, const media_stream* prev_stream)
{
    this->lock();

    const sample_t& sample = static_cast<const sample_t&>(sample_);
    typename request_queue::request_t* request = this->requests.get(rp.packet_number);
    assert_(request);

    /*Sleep(10);*/

    // find the right packet from the list and assign the sample to it
    bool found = false;
    for(auto&& item : request->sample.container)
        if(item.input_stream == prev_stream && item.sample.timestamp == -1)
        {
            request->sample.samples_received++;
            item.sample = sample;
            found = true;
            break;
        };
    assert_(found);

    // dispatch all requests that are ready
    for(typename request_queue::request_t* next_request = this->requests.get();
        next_request && 
        (size_t)next_request->sample.samples_received == this->input_streams_props.size();
        next_request = this->requests.get())
    {
        assert_(next_request->stream == this);

        typename request_queue::request_t request;
        this->requests.pop(request);

        this->process(request);

        this->lock();
    }

    this->unlock();
    return OK;
}