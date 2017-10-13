#include "media_session.h"
#include "media_sink.h"

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
    presentation_clock_t clock;
    if(!this->get_current_clock(clock))
        return false;

    return clock->clock_start(time_start);
}

bool media_session::stop_playback()
{
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
            scoped_lock lock(this->mutex);
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
        if((*jt)->request_sample(rp) == media_stream::FATAL_ERROR)
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

    media_topology_t topology(rp.topology);

    if(!topology)
        return false;

    media_topology::topology_t::iterator it = topology->topology.find(stream);
    if(it == topology->topology.end())
        return false;

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->process_sample(sample_view, rp) == media_stream::FATAL_ERROR)
            return false;

    return true;
}

void media_session::shutdown()
{
    media_topology_t null_topology;
    std::atomic_exchange(&this->current_topology, null_topology);
    std::atomic_exchange(&this->new_topology, null_topology);
}