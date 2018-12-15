#include "media_session.h"
#include "media_sink.h"
#include <Mferror.h>
#include <iostream>
#include "assert.h"

media_session::media_session(const presentation_time_source_t& time_source) :
    time_source(time_source)
{
}

media_session::~media_session()
{
}

bool media_session::get_current_topology(media_topology_t& topology) const
{
    topology = std::atomic_load(&this->current_topology);
    return !!topology;
}

bool media_session::get_current_clock(presentation_clock_t& clock) const
{
    media_topology_t topology;

    if(!this->get_current_topology(topology))
        return false;

    clock = topology->clock;
    return !!clock;
}

void media_session::switch_topology(const media_topology_t& topology)
{
    scoped_lock lock(this->topology_switch_mutex);
    this->new_topology = topology;
}

void media_session::switch_topology_immediate(const media_topology_t& new_topology, time_unit time_point)
{
    scoped_lock lock(this->topology_switch_mutex);

    /*if(this->is_shutdown)
        throw HR_EXCEPTION(E_UNEXPECTED);*/

    this->switch_topology(new_topology);

    media_topology_t topology = this->new_topology;

    // reset the new topology to null
    this->new_topology = NULL;

    // copy the packet number from old topology if it exists
    media_topology_t old_topology = std::atomic_load(&this->current_topology);
    if(old_topology)
    {
        topology->first_packet_number = old_topology->packet_number;
        topology->packet_number = topology->first_packet_number;
    }

    // acquire old clock
    presentation_clock_t clock;
    this->get_current_clock(clock);

    // switch to the new topology
    std::atomic_exchange(&this->current_topology, topology);

    // acquire new clock
    presentation_clock_t new_clock;
    if(!this->get_current_clock(new_clock))
        throw HR_EXCEPTION(E_UNEXPECTED);

    // the new topology will be active on clock events
    new_clock->clock_start(time_point, clock);
}

void media_session::start_playback(const media_topology_t& topology, time_unit time_point)
{
    /*this->is_started = true;*/
    this->switch_topology_immediate(topology, time_point);
}

bool media_session::request_sample(
    const media_stream* stream, 
    request_packet& rp,
    bool is_sink)
{
    media_topology::topology_t::iterator it;
    if(is_sink)
    {
        assert_(!rp.topology);

        // if the call is coming from sink, it is assumed that no subsequent is_sink
        // calls are made before this has been processed

        // packet numbers must be consecutive and newer packet number must correspond
        // to the newest topology;
        // after the clock stop call, the stream won't be able to dispatch new requests
        // because the topology isn't active anymore

        scoped_lock lock(this->topology_switch_mutex);

        this->get_current_topology(rp.topology);
        it = rp.topology->topology_reverse.find(stream);
        if(it == rp.topology->topology_reverse.end())
            return false;

        rp.packet_number = rp.topology->packet_number++;
        /*std::cout << rp.packet_number << std::endl;*/

        // check if there's a topology switch and switch the session to it
        if(this->new_topology)
        {
            this->switch_topology_immediate(this->new_topology, rp.timestamp);
            std::cout << "topology switched" << std::endl;
        }
    }
    else
    {
        assert_(rp.topology);
        it = rp.topology->topology_reverse.find(stream);
    }

    assert_(it != rp.topology->topology_reverse.end());

    for(auto jt = it->second.next.begin(); jt != it->second.next.end(); jt++)
        if((*jt)->request_sample(rp, stream) == media_stream::FATAL_ERROR)
            return false;

    return true;
}

bool media_session::give_sample(
    const media_stream* stream, 
    const media_sample& sample_view, 
    request_packet& rp,
    bool /*is_source*/)
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