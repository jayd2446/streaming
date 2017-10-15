#include "media_session.h"
#include "media_sink.h"
#include <Mferror.h>

media_session::media_session()
{
    this->give_sample_callback.Attach(new async_callback_t(&media_session::give_sample_cb));
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
    // assign a topology for the request packet if it's coming from the sink
    media_topology_t topology;
    if(is_sink)
    {
        assert(!rp.topology);

        topology = std::atomic_load(&this->new_topology);
        // check if there's a topology switch
        if(topology)
        {
            // lock is here so that the message passing doesn't become a bottleneck
            scoped_lock lock(this->switch_topology_mutex);
            if(!(topology = std::atomic_load(&this->new_topology)))
                return false;

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
    }
    topology = rp.topology;

    if(!topology)
        return false;

    media_topology::topology_t::iterator it = topology->topology_reverse.find(stream);
    if(it == topology->topology_reverse.end())
        return false;

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
    // is_source is used for translating time stamps to presentation time
    {
        give_sample_t request;
        request.stream = stream;
        request.sample_view = sample_view;
        request.rp = rp;
        request.is_source = is_source;

        scoped_lock lock(this->give_sample_mutex);
        this->give_sample_requests.push(request);
    }

    // dispatch this function to a work queue
    const HRESULT hr = this->give_sample_callback->mf_put_work_item(
        this->shared_from_this<media_session>(), MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();

    /*media_topology_t topology(rp.topology);

    if(!topology)
        return false;

    media_topology::topology_t::iterator it = topology->topology.find(stream);
    if(it == topology->topology.end())
        return false;

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->process_sample(sample_view, rp) == media_stream::FATAL_ERROR)
            return false;*/

    return true;
}

void media_session::give_sample_cb(void*)
{
    give_sample_t request;
    {
        scoped_lock lock(this->give_sample_mutex);
        request = this->give_sample_requests.front();
        this->give_sample_requests.pop();
    }

    media_topology_t topology(request.rp.topology);

    if(!topology)
        return;

    media_topology::topology_t::iterator it = topology->topology.find(request.stream);
    if(it == topology->topology.end())
        return;

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->process_sample(
            request.sample_view, request.rp, request.stream) == media_stream::FATAL_ERROR)
            return;
}

void media_session::shutdown()
{
    media_topology_t null_topology;
    std::atomic_exchange(&this->current_topology, null_topology);
    std::atomic_exchange(&this->new_topology, null_topology);
}