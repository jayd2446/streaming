#pragma once

#include "media_component.h"
#include "media_stream.h"
#include "media_sample.h"
#include "request_packet.h"
#include "request_dispatcher.h"
#include "request_queue_handler.h"
#include "assert.h"
#include <utility>
#include <memory>
#include <vector>
#include <limits>
#include <algorithm>
#include <mutex>
#include <iostream>

#define TRANSFORM_MIXER_APPLY_STREAM_CONTROLLER_IMMEDIATELY

#undef min
#undef max

// on stream stop the component should serve requests to the request point, so that
// a component that might stop receives enough samples;
// at other times, a component is allowed serve samples from zero up to the request point

// arg with empty sample indicates a frame skip;
// frame with empty buffer indicates a silent frame

// a request tagged with last packet might have null args

// the user params apply to a request only, which means that they might lag behind
// if the same parameters are applied to samples which have different timestamps

// TODO: investigate reserve calls
// TODO: derived class shouldn't specify the optional type;
// it should be internal to mixer

template<class TransformMixer> 
class stream_mixer;

// in arg and out arg are assumed to be of std optional type
template<class InArg, class UserParamsController, class OutArg>
class transform_mixer : public media_component
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
protected:
    virtual stream_mixer_t create_derived_stream() = 0;
public:
    explicit transform_mixer(const media_session_t& session);
    virtual ~transform_mixer() {}

    stream_mixer_t create_stream(media_message_generator_t&& message_generator);
};

template<class TransformMixer>
class stream_mixer : 
    public media_stream_message_listener, 
    request_queue_handler<typename TransformMixer::request_t>
{
    using media_stream::connect_streams;
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef TransformMixer transform_mixer;
    typedef std::shared_ptr<transform_mixer> transform_mixer_t;
    typedef typename transform_mixer::in_arg_t in_arg_t;
    typedef typename transform_mixer::out_arg_t out_arg_t;
    typedef typename transform_mixer::user_params_controller_t user_params_controller_t;
    typedef typename transform_mixer::user_params_t user_params_t;
    typedef typename transform_mixer::packet_t packet_t;
    typedef typename transform_mixer::args_t args_t;
    typedef typename request_queue_handler<typename TransformMixer::request_t>::request_t 
        request_t;
    typedef typename request_queue_handler<typename TransformMixer::request_t>::request_queue 
        request_queue;
    typedef media_stream::result_t result_t;

    struct input_stream_props_t
    {
        media_stream* input_stream;
        user_params_controller_t user_params_controller;
    };
    struct dispatcher_args_t
    {
        args_t packets; 
        frame_unit old_cutoff, cutoff;
    };

    typedef request_dispatcher<typename ::request_queue<dispatcher_args_t>::request_t> 
        request_dispatcher;
private:
    transform_mixer_t transform;
    time_unit drain_point;
    std::vector<input_stream_props_t> input_streams_props;
    std::shared_ptr<request_dispatcher> dispatcher;
    std::mutex next_request_mutex;

    // frames below cutoff are dismissed
    frame_unit cutoff;
    std::unique_ptr<args_t[]> leftover;

    // converts by using the frame rate in component
    frame_unit convert_to_frame_unit(time_unit) const;
    void initialize_packet(packet_t&) const;
    frame_unit find_common_frame_end(const args_t&, frame_unit lower_limit) const;
    void process(typename request_queue::request_t&);
    void dispatch(typename request_dispatcher::request_t&);

    // request_queue_handler
    bool on_serve(typename request_queue::request_t&) override;
    typename request_queue::request_t* next_request() override;
protected:
    virtual void on_stream_start(time_unit) override;
    virtual void on_stream_stop(time_unit) override;
    // moves all frames from 'from' to 'to', using the 'reference' as the source for samples,
    // and updates the fields of 'from' and 'to';
    // 'reference' is assumed to be valid;
    // returns whether 'from' became null(all frames were moved);
    // discarded flag indicates whether the sample is immediately discarded
    virtual bool move_frames(in_arg_t& to, in_arg_t& from, const in_arg_t& reference,
        frame_unit end, bool discarded) = 0;
    // mixes all the frames in args to out up to end;
    // the samples in args shouldn't be modified;
    // NOTE: mixing must be multithreading safe
    virtual void mix(out_arg_t& out, args_t&, frame_unit first, frame_unit end) = 0;
public:
    explicit stream_mixer(const transform_mixer_t& transform);
    virtual ~stream_mixer() {}

    size_t get_input_stream_count() const {return this->input_streams_props.size();}

    // user_params can be NULL
    void connect_streams(
        const media_stream_t& from,
        const user_params_controller_t& user_params_controller,
        const media_topology_t&);

    result_t request_sample(const request_packet&, const media_stream*) override final;
    result_t process_sample(
        const media_component_args*, const request_packet&, const media_stream*) override final;
};


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<class T, class U, class V>
transform_mixer<T, U, V>::transform_mixer(const media_session_t& session) :
    media_component(session)
{
}

template<class T, class U, class V>
typename transform_mixer<T, U, V>::stream_mixer_t transform_mixer<T, U, V>::create_stream(
    media_message_generator_t&& message_generator)
{
    stream_mixer_t stream = this->create_derived_stream();
    stream->register_listener(message_generator);
    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<class T>
stream_mixer<T>::stream_mixer(const transform_mixer_t& transform) :
    media_stream_message_listener(transform.get()),
    transform(transform),
    drain_point(std::numeric_limits<time_unit>::min()),
    cutoff(std::numeric_limits<time_unit>::min()),
    dispatcher(new request_dispatcher)
{
}

template<class T>
void stream_mixer<T>::on_stream_start(time_unit t)
{
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
frame_unit stream_mixer<T>::convert_to_frame_unit(time_unit t) const
{
    return ::convert_to_frame_unit(t,
        this->transform->session->frame_rate_num,
        this->transform->session->frame_rate_den);
}

template<class T>
void stream_mixer<T>::initialize_packet(packet_t& packet) const
{
    packet.input_stream = this->input_streams_props[packet.stream_index].input_stream;
    packet.valid_user_params = !!this->input_streams_props[packet.stream_index].user_params_controller;
    if(packet.valid_user_params)
        this->input_streams_props[packet.stream_index].user_params_controller->get_params(
            packet.user_params);
}

template<class T>
frame_unit stream_mixer<T>::find_common_frame_end(const args_t& args, frame_unit lower_limit) const
{
    assert_(args.container.size() == this->input_streams_props.size());

    frame_unit frame_end = std::numeric_limits<frame_unit>::max();
    for(size_t i = 0; i < this->input_streams_props.size(); i++)
    {
        frame_unit leftover_frame_end = std::numeric_limits<frame_unit>::min();
        for(const auto& item : this->leftover[i].container)
        {
            assert_(item.arg);

            const frame_unit item_frame_end = std::max(lower_limit, item.arg->frame_end);
            leftover_frame_end = std::max(leftover_frame_end, item_frame_end);
        }

        const auto& item = args.container[i];
        if(item.arg)
        {
            const frame_unit item_frame_end = std::max(lower_limit, item.arg->frame_end);
            frame_end = std::min(frame_end, std::max(item_frame_end, leftover_frame_end));
        }
        else if(!this->leftover[i].container.empty())
            frame_end = std::min(frame_end, leftover_frame_end);
        else
            // common frame end cannot be found if a request is lacking samples
            return lower_limit;
    }

    return frame_end;
}

template<class T>
void stream_mixer<T>::process(typename request_queue::request_t& request)
{
    assert_(request.sample.second.container.size() == this->input_streams_props.size());

    // TODO: decide if transform mixer should have a limit for the difference between
    // the old cutoff and the current one;
    // old cutoff could be increased if the limit is reached

    args_t& packets = request.sample.second;
    const frame_unit old_cutoff = this->cutoff;
    this->cutoff = this->find_common_frame_end(packets, old_cutoff);
    // component is allowed serve samples from zero up to the request point only
    assert_(this->cutoff <= this->convert_to_frame_unit(request.rp.request_time));

    if(request.rp.flags & FLAG_LAST_PACKET)
    {
        const frame_unit drain_cutoff = this->convert_to_frame_unit(request.rp.request_time);
        // the initialized value of new_cutoff(=this->cutoff) above should be either lower or equal 
        // to drain point, since the sources are allowed to serve samples up to the 
        // request point only;
        // for no gaps in the stream, the drain cutoff should be the same as new_cutoff
        assert_(drain_cutoff >= this->cutoff);
        if(drain_cutoff > this->cutoff)
            std::cout << "warning: a source didn't serve samples up to the drain point" << std::endl;
        this->cutoff = drain_cutoff;
        std::cout << "drain on mixer" << std::endl;
    }

    // move the packets to the leftover container
    for(auto&& item : packets.container)
    {
        if(item.arg)
            this->leftover[item.stream_index].container.push_back(std::move(item));
    }
    packets.container.clear();

    // this is an optimization so that the pipeline doesn't cause too much overhead if
    // a source has a lower fps than the pipeline
    if(this->cutoff == old_cutoff)
        goto out;

    for(size_t i = 0; i < this->input_streams_props.size(); i++)
    {
        auto& container = this->leftover[i].container;

        container.erase(std::remove_if(container.begin(), container.end(),
            [&](packet_t& item)
            {
                bool remove_item = true;
                in_arg_t modified;

                // try discarding old items
                if(item.arg)
                {
                    in_arg_t discarded;
                    const bool all_frames_moved =
                        this->move_frames(discarded, modified, item.arg, old_cutoff, true);

                    assert_((all_frames_moved && !modified) || (!all_frames_moved && modified));
                }

                // try modifying the leftover buffer
                in_arg_t new_arg;
                if(modified)
                {
                    in_arg_t modified2;
                    const bool all_frames_moved =
                        this->move_frames(new_arg, modified2, modified, this->cutoff, false);

                    // TODO: this could be further optimized so that
                    // item.arg = modified2 won't be performed if item.arg == modified2;
                    // happens only when nothing is moved to 'to'
                    if(!all_frames_moved)
                    {
                        item.arg = modified2;

                        // keep the item in the leftover buffer since it was not fully moved
                        remove_item = false;
                    }

                    assert_((all_frames_moved && !modified2) || (!all_frames_moved && modified2));
                }

                // try assigning to the request
                if(new_arg && new_arg->sample)
                {
#ifdef TRANSFORM_MIXER_APPLY_STREAM_CONTROLLER_IMMEDIATELY
                    // apply stream controller here;
                    // it overwrites the previous value set in request_sample;
                    // get_params is multithread safe
                    if(item.valid_user_params)
                        this->input_streams_props[item.stream_index].
                        user_params_controller->get_params(item.user_params);
#endif

                    // move the processed packet to the request
                    packet_t new_item = item;
                    new_item.arg = new_arg;
                    packets.container.push_back(std::move(new_item));
                }

                return remove_item;

            }), container.end());
    }

out:
    const frame_unit cutoff = this->cutoff;
    assert_(old_cutoff <= this->cutoff);

    // only mix if there is something to mix
    if(old_cutoff != cutoff)
    {
        typename request_dispatcher::request_t dispatcher_request;
        dispatcher_request.stream = request.stream;
        dispatcher_request.rp = request.rp;
        dispatcher_request.sample.packets = std::move(packets);
        dispatcher_request.sample.cutoff = cutoff;
        dispatcher_request.sample.old_cutoff = old_cutoff;

        this->dispatcher->dispatch_request(std::move(dispatcher_request),
            std::bind(&stream_mixer::dispatch, this->shared_from_this<stream_mixer>(), 
                std::placeholders::_1));
    }
    else
    {
        // pass null args downstream
        typename request_dispatcher::request_t dispatcher_request;
        dispatcher_request.stream = request.stream;
        dispatcher_request.rp = request.rp;

        this->dispatcher->dispatch_request(std::move(dispatcher_request),
            [this_ = this->shared_from_this<stream_mixer>()](
                typename request_dispatcher::request_t& request)
        {
            this_->transform->session->give_sample(request.stream, NULL, request.rp);
        });
    }
}

template<class T>
void stream_mixer<T>::dispatch(typename request_dispatcher::request_t& request)
{
    const frame_unit cutoff = request.sample.cutoff,
        old_cutoff = request.sample.old_cutoff;

    assert_(cutoff != old_cutoff);

    out_arg_t out;
    this->mix(out, request.sample.packets, old_cutoff, cutoff);

    this->transform->session->give_sample(request.stream,
        out.has_value() ? &(*out) : NULL, request.rp);
}

template<class T>
bool stream_mixer<T>::on_serve(typename request_queue::request_t& request)
{
    this->process(request);
    return true;
}

template<class T>
typename stream_mixer<T>::request_queue::request_t* stream_mixer<T>::next_request()
{
    scoped_lock lock(this->next_request_mutex);

    typename request_queue::request_t* request = this->requests.get();
    if(request && (size_t)request->sample.first == this->input_streams_props.size())
        return request;
    else
        return NULL;
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
        this->requests.push(std::move(request));
    }
    typename request_queue::request_t* request = this->requests.get(rp.packet_number);
    assert_(request->sample.second.container.empty());

    request->sample.first = 0;
    args_t& packets = request->sample.second;

    // TODO: the current leftover size should be added to the reservation
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
    typename request_queue::request_t* request = this->requests.get(rp.packet_number);
    assert_(request);

    /*Sleep(10);*/

    // find the right packet from the list and assign the sample to it
    bool found = false;
    {
        scoped_lock lock(this->next_request_mutex);
        for(auto&& item : request->sample.second.container)
            if(item.input_stream == prev_stream && !item.arg)
            {
                request->sample.first++;
                if(arg_)
                    item.arg = std::make_optional(static_cast<const in_arg_t::value_type&>(*arg_));
                found = true;
                break;
            }
    }
    assert_(found);

    // dispatch all requests that are ready
    this->serve();

    return OK;
}