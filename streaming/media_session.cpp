#include "media_session.h"
#include "media_sink.h"

presentation_clock_t media_session::get_current_clock() const
{
    if(!this->current_topology)
        return NULL;
    return this->current_topology->clock;
}

void media_session::switch_topology(const media_topology_t& topology)
{
    // TODO: implement topology switching(includes copying the current time from the old topology);
    // (includes stopping the current topology)
    std::atomic_exchange(&this->current_topology, topology);
}

bool media_session::start_playback(time_unit time_start)
{
    presentation_clock_t clock = this->get_current_clock();
    if(!clock)
        return false;

    return clock->clock_start();
}

bool media_session::stop_playback()
{
    presentation_clock_t clock = this->get_current_clock();
    if(!clock)
        return false;

    clock->clock_stop();
}

bool media_session::request_sample(const media_stream* stream, bool is_sink) const
{
    // is_sink is used for switching to a new topology

    // take reference of the current topology because the topology switch might happen here
    media_topology_t topology;
    std::atomic_exchange(&topology, this->current_topology);

    if(!topology)
        return false;

    media_topology::topology_t::iterator it = topology->topology_reverse.find(stream);
    if(it == topology->topology_reverse.end())
        return false;

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->request_sample() == media_stream::FATAL_ERROR)
            return false;

    return true;
}

bool media_session::give_sample(
    const media_stream* stream, const media_sample_t& sample, bool is_source) const
{
    // is_source is used for translating time stamps to presentation time

    // take reference of the current topology because the topology switch might happen here
    media_topology_t topology;
    std::atomic_exchange(&topology, this->current_topology);

    if(!topology)
        return false;

    media_topology::topology_t::iterator it = topology->topology.find(stream);
    if(it == topology->topology.end())
        return false;

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->process_sample(sample) == media_stream::FATAL_ERROR)
            return false;

    return true;
}

void media_session::shutdown()
{
    this->current_topology = this->new_topology = NULL;
}