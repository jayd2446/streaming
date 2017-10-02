#include "media_session.h"
#include "media_sink.h"

bool media_session::get_current_clock(presentation_clock_t& clock) const
{
    media_topology_t topology(std::atomic_load(&this->current_topology));

    if(!topology)
        return false;

    std::atomic_exchange(&clock, topology->clock);
    return true;
}

void media_session::switch_topology(const media_topology_t& topology)
{
    // TODO: implement topology switching(includes copying the current time from the old topology);
    // (includes stopping the current topology)
    // (includes clearing the clock sink from the old clock)
    std::atomic_exchange(&this->current_topology, topology);
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
    bool is_sink) const
{
    // assign a new topology for the request packet if it's coming from the sink
    media_topology_t topology;
    if(is_sink)
        rp.topology = std::atomic_load(&this->current_topology);
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
    const media_sample_t& sample, 
    request_packet& rp,
    bool is_source) const
{
    // is_source is used for translating time stamps to presentation time

    media_topology_t topology(rp.topology);

    if(!topology)
        return false;

    media_topology::topology_t::iterator it = topology->topology.find(stream);
    if(it == topology->topology.end())
        return false;

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->process_sample(sample, rp) == media_stream::FATAL_ERROR)
            return false;

    return true;
}

void media_session::shutdown()
{
    media_topology_t null_topology;
    std::atomic_exchange(&this->current_topology, null_topology);
}