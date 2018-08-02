#include "media_stream.h"
#include "media_topology.h"
#include "presentation_clock.h"
#include "assert.h"

void media_stream::connect_streams(const media_stream_t& from, const media_topology_t& topology)
{
    topology->connect_streams(from, this->shared_from_this<media_stream>());
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_stream_clock_sink::media_stream_clock_sink(const media_component* component) : 
    component(component), unregistered(true)
{
}

void media_stream_clock_sink::register_sink(presentation_clock_t& clock)
{
    assert_(this->unregistered);

    clock->register_sink(this->shared_from_this<media_stream_clock_sink>(), this->component);
    this->unregistered = false;
}