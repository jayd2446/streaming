#include "media_session.h"

void media_session::switch_topology(const media_topology_t& topology)
{
    // TODO: implement topology switching
    this->current_topology = topology;
}

bool media_session::request_sample(const media_stream* stream, bool is_sink) const
{
    // is_sink is used for switching to a new topology

    media_topology::topology_t::iterator it = this->current_topology->topology_reverse.find(stream);
    if(it == this->current_topology->topology_reverse.end())
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

    media_topology::topology_t::iterator it = this->current_topology->topology.find(stream);
    if(it == this->current_topology->topology.end())
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