#include "media_stream.h"
#include "media_topology.h"
#include "presentation_clock.h"
#include "assert.h"

media_stream::media_stream(stream_t stream_type) : 
    stream_type(stream_type), locked(false)
{
}

void media_stream::connect_streams(const media_stream_t& from, const media_topology_t& topology)
{
    topology->connect_streams(from, this->shared_from_this<media_stream>());
}

void media_stream::lock()
{
    scoped_lock lock(this->mutex);
    while(this->locked)
        this->cv.wait(lock);
    this->locked = true;
}

void media_stream::unlock()
{
    scoped_lock lock(this->mutex);
    assert_(this->locked);
    this->locked = false;
    this->cv.notify_one();
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