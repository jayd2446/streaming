#include "media_session.h"
#include "media_sink.h"
#include "media_stream.h"
#include <Mferror.h>
#include <iostream>
#include "assert.h"

#pragma warning(push)
#pragma warning(disable: 4706) // assignment within conditional expression

media_session::media_session(const media_clock_t& time_source,
    frame_unit frame_rate_num, frame_unit frame_rate_den) :
    time_source(time_source),
    request_chain_lock(request_chain_mutex, std::defer_lock),
    frame_rate_num(frame_rate_num), frame_rate_den(frame_rate_den)
{
    if(this->frame_rate_num <= 0 || this->frame_rate_den <= 0)
        throw HR_EXCEPTION(E_UNEXPECTED);
}

media_topology_t media_session::get_current_topology() const
{
    if(!this->request_chain_lock.owns_lock())
        throw HR_EXCEPTION(E_UNEXPECTED);

    return this->current_topology;
}

void media_session::switch_topology(const media_topology_t& topology)
{
    std::atomic_store(&this->new_topology, topology);
}

void media_session::switch_topology_immediate(
    const media_topology_t& new_topology, time_unit time_point)
{
    assert_(this->request_chain_lock.owns_lock());

    // increment the topology number if the old topology exists
    media_topology_t old_topology = this->current_topology;
    if(old_topology)
        new_topology->topology_number = old_topology->topology_number + 1;

    // acquire old message generator
    media_message_generator_t message_generator;
    if(this->current_topology)
        message_generator = this->current_topology->get_message_generator();

    // switch to the new topology
    this->current_topology = new_topology;

    // acquire new message generator
    media_message_generator_t new_message_generator = this->current_topology->get_message_generator();
    if(!new_message_generator)
        throw HR_EXCEPTION(E_UNEXPECTED);

    // the new topology will be activated
    new_message_generator->clock_start(time_point, message_generator);
}

void media_session::start_playback(const media_topology_t& topology, time_unit time_point)
{
    // the request chain lock here really isn't necessary, but it is assumed by the
    // called function
    this->request_chain_lock.lock();
    this->switch_topology_immediate(topology, time_point);
    this->request_chain_lock.unlock();
}

bool media_session::request_sample(const media_stream* stream, const request_packet& rp)
{
    assert_(this->request_chain_lock.owns_lock());

    media_topology_t topology(rp.topology);
    assert_(topology);

    media_topology::topology_t::iterator it = topology->topology_reverse.find(stream);
    assert_(it != rp.topology->topology_reverse.end());

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
    {
        // request sample calls for sources are made after all component calls for making sure that
        // no process_sample calls begin before request_sample calls
        if((*jt)->is_source_stream())
            continue;

        if((*jt)->request_sample(rp, stream) == media_stream::FATAL_ERROR)
            return false;
    }

    return true;
}

bool media_session::give_sample(
    const media_stream* stream, 
    const media_component_args* args,
    const request_packet& rp)
{
    // TODO: media topology should be defined as const

    media_topology_t topology(rp.topology);
    assert_(topology);

    media_topology::topology_t::iterator it = topology->topology.find(stream);
    assert_(it != topology->topology.end());

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->process_sample(args, rp, stream) == media_stream::FATAL_ERROR)
            return false;

    return true;
}

bool media_session::begin_request_sample(media_stream* stream, const request_packet& incomplete_rp, 
    const media_topology_t& topology)
{
    assert_(!stream->is_source_stream());

    request_packet rp = incomplete_rp;

    // TODO: it must be ensured that the topology switching functionality is restricted to
    // single thread

    assert_(!rp.topology);

    // this mutex ensures that the begin request sample call chain is atomic
    this->request_chain_lock.lock();

    rp.topology = topology;
    if(rp.topology->topology_reverse.find(stream) == rp.topology->topology_reverse.end())
    {
        this->request_chain_lock.unlock();
        return false;
    }

    rp.packet_number = rp.topology->next_packet_number++;
    /*std::cout << rp.packet_number << std::endl;*/

    // check if there's a topology switch and switch the session to it;
    // topology switch can only be triggered by the current topology
    if(topology == this->current_topology)
    {
        media_topology_t new_topology = std::atomic_exchange(&this->new_topology, media_topology_t());
        if(new_topology)
        {
            this->switch_topology_immediate(new_topology, rp.request_time);
            std::cout << "topology switched" << std::endl;
        }
    }

    // if the topology isn't current, it is in a state of being stopped
    if(topology != this->current_topology)
    {
        if(rp.topology->drained || 
            (rp.topology->drained = rp.topology->get_message_generator()->is_drainable(rp.request_time)))
            // this is the final request
            rp.flags |= FLAG_LAST_PACKET;
    }

    bool ret = (stream->request_sample(rp, NULL) != media_stream::FATAL_ERROR);

    // call request_sample for sources
    for(auto&& item : rp.topology->source_streams)
        if(!ret || (item->request_sample(rp, NULL) == media_stream::FATAL_ERROR))
        {
            ret = false;
            break;
        }

    this->request_chain_lock.unlock();

    return ret;
}

#pragma warning(pop)