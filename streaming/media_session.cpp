#include "media_session.h"
#include "media_sink.h"
#include <Mferror.h>
#include <cassert>

media_session::media_session()
{
    this->give_sample_callback.Attach(new async_callback_t(&media_session::give_sample_cb));
    this->request_sample_callback.Attach(new async_callback_t(&media_session::request_sample_cb));

    // TODO: remove serial work queue
    if(FAILED(MFAllocateSerialWorkQueue(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &this->serial_workqueue_id)))
        throw std::exception();
}

media_session::~media_session()
{
    const HRESULT hr = MFUnlockWorkQueue(this->serial_workqueue_id);
    assert(SUCCEEDED(hr));
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
    if(!std::atomic_load(&this->current_topology))
        std::atomic_exchange(&this->current_topology, topology);
    else
        std::atomic_exchange(&this->new_topology, topology);
}

bool media_session::start_playback(time_unit time_start)
{
    // TODO: add parameter for playback topology
    // (so that switch_topology doesnt have to set it)

    presentation_clock_t clock;
    if(!this->get_current_clock(clock))
        return false;

    return clock->clock_start(time_start);
}

bool media_session::stop_playback()
{
    // TODO: stop_playback might in turn automatically set the topology
    // to null

    presentation_clock_t clock;
    if(!this->get_current_clock(clock))
        return false;

    clock->clock_stop();
    return true;
}

bool media_session::request_sample(
    const media_stream* stream, 
    request_packet& rp,
    bool is_sink)
{
    // dispatch the request call to a work queue if it's coming from a sink
    media_topology_t topology;
    if(is_sink)
    {
        // if the call is coming from sink, it is assumed that no subsequent is_sink
        // calls are made before this has been processed

        assert(!rp.topology);

        // check if there's a topology switch
        topology = std::atomic_load(&this->new_topology);
        if(topology)
        {
            presentation_clock_t clock, old_clock;

            // TODO: for even more precise set_current_time,
            // the old clock shouldnt be stopped at all
            // (requires a temporary clock to be set)

            // rp.topology is null when it's coming from the sink
            std::atomic_exchange(&this->new_topology, rp.topology);
            // stop the current topology
            if(!this->get_current_clock(old_clock))
                throw std::exception();
            old_clock->clock_stop();
            // switch to the new topology
            std::atomic_exchange(&this->current_topology, topology);
            // start the new topology at the previous clock time
            if(!this->get_current_clock(clock))
                throw std::exception();
            // sets the clock to old time and starts the timer
            // with the request time stamp
            clock->set_current_time(old_clock->get_current_time());
            clock->clock_start(rp.timestamp, false, rp.packet_number);

            return false;
        }

        rp.topology = std::atomic_load(&this->current_topology);

        //request_sample_t request;
        //request.stream = stream;
        //request.rp = rp;

        //{
        //    scoped_lock lock(this->request_sample_mutex);
        //    this->request_sample_requests.push(request);
        //}

        //// dispatch this function to a work queue
        //const HRESULT hr = this->request_sample_callback->mf_put_work_item(
        //    this->shared_from_this<media_session>(), this->serial_workqueue_id);
        //if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        //    throw std::exception();
        //return true;
    }

    topology = rp.topology;
    assert(topology);

    media_topology::topology_t::iterator it = topology->topology_reverse.find(stream);
    assert(it != topology->topology_reverse.end());

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
    media_topology_t null_topology;
    std::atomic_exchange(&this->current_topology, null_topology);
    std::atomic_exchange(&this->new_topology, null_topology);
}