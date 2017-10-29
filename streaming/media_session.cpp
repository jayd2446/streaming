#include "media_session.h"
#include "media_sink.h"
#include <Mferror.h>
#include <cassert>

media_session::media_session()
{
    this->give_sample_callback.Attach(new async_callback_t(&media_session::give_sample_cb));
    this->request_sample_callback.Attach(new async_callback_t(&media_session::request_sample_cb));
}

media_session::~media_session()
{
}

bool media_session::switch_topology_immediate(const media_topology_t& new_topology, time_unit time_point)
{
    scoped_lock lock(this->topology_switch_mutex);

    this->switch_topology(new_topology);

    // topologies are assumed to share the same time source
    presentation_clock_t clock;
    media_topology_t topology = this->new_topology;

    // reset the new topology to null
    this->new_topology = NULL;

    // stop the old topology and copy its packet number if it exists
    if(this->get_current_clock(clock))
    {
        clock->clock_stop(time_point);

        media_topology_t old_topology = std::atomic_load(&this->current_topology);
        topology->packet_number = old_topology->packet_number;
    }

    // switch to the new topology
    std::atomic_exchange(&this->current_topology, topology);

    // start the new topology
    if(!this->get_current_clock(clock))
        throw std::exception();
    return clock->clock_start(time_point);
}

bool media_session::get_current_clock(presentation_clock_t& clock) const
{
    media_topology_t topology(std::atomic_load(&this->current_topology));

    if(!topology)
        return false;

    clock = topology->clock;
    return !!clock;
}

void media_session::switch_topology(const media_topology_t& topology)
{
    scoped_lock lock(this->topology_switch_mutex);
    this->new_topology = topology;
}

bool media_session::start_playback(const media_topology_t& topology, time_unit time_point)
{
    return this->switch_topology_immediate(topology, time_point);
}

void media_session::stop_playback()
{
    // TODO: stop play back should switch to a topology that has the same time source
    // but no streams

    presentation_clock_t clock;
    if(!this->get_current_clock(clock))
        throw std::exception();

    clock->clock_stop();
}



#include <iostream>

bool media_session::request_sample(
    const media_stream* stream, 
    request_packet& rp,
    bool is_sink)
{
    if(is_sink)
    {
        // if the call is coming from sink, it is assumed that no subsequent is_sink
        // calls are made before this has been processed

        // packet numbers must be consecutive and newer packet number must correspond
        // to the newest topology;
        // after the clock stop call, the stream won't be able to dispatch new requests
        // because the topology isn't active anymore

        this->topology_switch_mutex.lock();

        assert(!rp.topology);

        // check if there's a topology switch
        if(this->new_topology)
        {
            this->switch_topology_immediate(this->new_topology, rp.timestamp);
            this->topology_switch_mutex.unlock();
            return false;
        }

        rp.topology = std::atomic_load(&this->current_topology);
    }

    assert(rp.topology);

    media_topology::topology_t::iterator it = rp.topology->topology_reverse.find(stream);
    assert(is_sink || it != rp.topology->topology_reverse.end());
    if(it == rp.topology->topology_reverse.end())
    {
        assert(is_sink);
        this->topology_switch_mutex.unlock();
        return false;
    }

    if(is_sink)
    {   
        rp.packet_number = rp.topology->packet_number++;
        /*std::cout << rp.packet_number << std::endl;*/
        this->topology_switch_mutex.unlock();
    }

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->request_sample(rp, stream) == media_stream::FATAL_ERROR)
            return false;

    return true;
}

bool media_session::give_sample(
    const media_stream* stream, 
    const media_sample_view_t& sample_view, 
    request_packet& rp,
    bool is_source)
{
    // TODO: media topology should be defined as const

    // is_source is used for translating time stamps to presentation time
    media_topology_t topology(rp.topology);
    assert(topology);

    media_topology::topology_t::iterator it = topology->topology.find(stream);
    assert(it != topology->topology.end());

    // dispatch to work queue if there are more downstream nodes than 1
    if(it->second.next.size() > 1)
    {
        for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        {
            // dispatch to work queue
            give_sample_t request;
            request.stream = stream;
            request.sample_view = sample_view;
            request.rp = rp;
            request.is_source = is_source;
            request.down_stream = (*jt).get();

            {
                scoped_lock lock(this->give_sample_mutex);
                this->give_sample_requests.push(request);
            }

            const HRESULT hr = this->give_sample_callback->mf_put_work_item(
                this->shared_from_this<media_session>(), MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
            if(FAILED(hr) && hr != MF_E_SHUTDOWN)
                throw std::exception();
        }
    }
    else
    {
        if(it->second.next.front()->process_sample(sample_view, rp, stream) == media_stream::FATAL_ERROR)
            return false;
    }

    /*if(it->second.next.size() > 1)*/
    //{
    //    // dispatch to work queue
    //    {
    //        give_sample_t request;
    //        request.stream = stream;
    //        request.sample_view = sample_view;
    //        request.rp = rp;
    //        request.is_source = is_source;
    //        request.it = it;

    //        scoped_lock lock(this->give_sample_mutex);
    //        this->give_sample_requests.push(request);
    //    }

    //    const HRESULT hr = this->give_sample_callback->mf_put_work_item(
    //        this->shared_from_this<media_session>(), MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
    //    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
    //        throw std::exception();
    //}
    /*else
    {
        for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
            if((*jt)->process_sample(sample_view, rp, stream) == media_stream::FATAL_ERROR)
                return false;
    }*/

    return true;
}

void media_session::request_sample_cb(void*)
{
    request_sample_t request;
    {
        scoped_lock lock(this->request_sample_mutex);
        request = this->request_sample_requests.front();
        this->request_sample_requests.pop();
    }

    media_topology_t topology(request.rp.topology);
    assert(topology);

    media_topology::topology_t::iterator it = topology->topology_reverse.find(request.stream);
    assert(it != topology->topology_reverse.end());

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->request_sample(request.rp, request.stream) == media_stream::FATAL_ERROR)
            return;
}

void media_session::give_sample_cb(void*)
{
    give_sample_t request;
    {
        scoped_lock lock(this->give_sample_mutex);
        request = this->give_sample_requests.front();
        this->give_sample_requests.pop();
    }

    if(request.down_stream->process_sample(
        request.sample_view, request.rp, request.stream) == media_stream::FATAL_ERROR)
        return;
}

void media_session::shutdown()
{
    scoped_lock lock(this->topology_switch_mutex);

    media_topology_t null_topology;
    std::atomic_exchange(&this->current_topology, null_topology);
    this->new_topology = null_topology;
}