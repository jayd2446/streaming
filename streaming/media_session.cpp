#include "media_session.h"
#include "media_sink.h"
#include "media_stream.h"
#include <Mferror.h>
#include <iostream>
#include "assert.h"

media_session::media_session(const presentation_time_source_t& time_source) :
    time_source(time_source),
    request_chain_lock(request_chain_mutex, std::defer_lock)
{
}

media_session::~media_session()
{
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

    // copy the packet number from old topology if it exists
    media_topology_t old_topology = this->current_topology;
    if(old_topology)
    {
        new_topology->first_packet_number = old_topology->packet_number;
        new_topology->packet_number = new_topology->first_packet_number;
    }

    // acquire old clock
    presentation_clock_t clock;
    if(this->current_topology)
        clock = this->current_topology->get_clock();

    // switch to the new topology
    this->current_topology = new_topology;

    // acquire new clock
    presentation_clock_t new_clock = this->current_topology->get_clock();
    if(!new_clock)
        throw HR_EXCEPTION(E_UNEXPECTED);

    // the new topology will be active on clock events
    new_clock->clock_start(time_point, clock);
}

void media_session::start_playback(const media_topology_t& topology, time_unit time_point)
{
    // the request chain lock here really isn't necessary, but it is assumed by the
    // called function
    this->request_chain_lock.lock();
    this->switch_topology_immediate(topology, time_point);
    this->request_chain_lock.unlock();
}

bool media_session::request_sample(const media_stream* stream, request_packet& rp)
{
    assert_(this->request_chain_lock.owns_lock());

    media_topology_t topology(rp.topology);
    assert_(topology);

    media_topology::topology_t::iterator it = topology->topology_reverse.find(stream);
    assert_(it != rp.topology->topology_reverse.end());

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->request_sample(rp, stream) == media_stream::FATAL_ERROR)
            return false;

    return true;
}

bool media_session::give_sample(
    const media_stream* stream, 
    const media_sample& sample_view, 
    request_packet& rp)
{
    // TODO: media topology should be defined as const

    media_topology_t topology(rp.topology);
    assert_(topology);

    media_topology::topology_t::iterator it = topology->topology.find(stream);
    assert_(it != topology->topology.end());

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->process_sample(sample_view, rp, stream) == media_stream::FATAL_ERROR)
            return false;

    return true;
}

bool media_session::begin_request_sample(const media_stream* stream,
    const request_packet& incomplete_rp)
{
    request_packet rp = incomplete_rp;

    assert_(!rp.topology);
    // packet numbers must be consecutive and newer packet number must correspond
    // to the newest topology;
    // after the clock stop call, the stream won't be able to dispatch new requests
    // because the topology isn't active anymore

    // this mutex ensures that the begin request sample call chain is atomic
    this->request_chain_lock.lock();

    rp.topology = this->get_current_topology();
    if(rp.topology->topology_reverse.find(stream) == rp.topology->topology_reverse.end())
        return NULL;

    rp.packet_number = rp.topology->packet_number++;
    /*std::cout << rp.packet_number << std::endl;*/

    // check if there's a topology switch and switch the session to it
    media_topology_t new_topology = std::atomic_exchange(&this->new_topology, media_topology_t());
    if(new_topology)
    {
        this->switch_topology_immediate(new_topology, rp.timestamp);
        std::cout << "topology switched" << std::endl;
    }

    const bool ret = this->request_sample(stream, rp);
    this->request_chain_lock.unlock();

    return ret;
}

void media_session::begin_give_sample(const media_stream* stream, 
    const media_topology_t& topology)
{
    // TODO: do not use dummy args;
    // instead, make those args optional
    assert_(topology);

    media_topology::topology_t::iterator it = topology->topology.find(stream);
    assert_(it != topology->topology.end());

    for(auto&& item : it->second.next)
    {
        media_sample dummy;
        request_packet dummy2;
        item->process_sample(dummy, dummy2, stream);
    }
}