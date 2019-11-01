#include "media_stream.h"
#include "media_topology.h"
#include "media_clock.h"
#include "assert.h"

media_stream::media_stream(stream_t stream_type) : locked(false), stream_type(stream_type)
{
}

void media_stream::connect_streams(const media_stream_t& from, const media_topology_t& topology)
{
    this->topology = topology;
    topology->connect_streams(from, this->shared_from_this<media_stream>());
}

media_topology_t media_stream::get_topology() const
{
    return this->topology.lock();
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


media_stream_message_listener::media_stream_message_listener(
    const media_component* component, stream_t stream_type) :
    media_stream(stream_type), component(component), unregistered(true)
{
}

void media_stream_message_listener::register_listener(const media_message_generator_t& message_generator)
{
    assert_(this->unregistered);

    message_generator->register_listener(this->shared_from_this<media_stream_message_listener>(), 
        this->component);
    this->unregistered = false;
}