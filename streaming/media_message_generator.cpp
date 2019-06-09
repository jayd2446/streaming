#include "media_message_generator.h"
#include "media_clock.h"
#include "media_stream.h"

media_message_generator::~media_message_generator()
{
    /*assert_(this->listeners.empty());*/
}

void media_message_generator::clock_start(time_unit time_point,
    const media_message_generator_t& prev_event_generator)
{
    if(!prev_event_generator)
    {
        this->clock_start(time_point);
        return;
    }

    /*assert_(this->get_time_source().get() == prev_clock->get_time_source().get());*/

    scoped_lock lock(this->mutex_listeners), lock2(prev_event_generator->mutex_listeners);
    for(auto it = prev_event_generator->listeners.begin(); it != prev_event_generator->listeners.end(); it++)
    {
        for(auto jt = this->listeners.begin(); jt != this->listeners.end(); jt++)
        {
            const bool transferred = (it->first == jt->first);
            it->second.second |= transferred;
            jt->second.second |= transferred;
        }

        // fire the clock sink events
        for(auto&& jt : it->second.first)
        {
            jt->on_stream_stop(time_point);
            if(!it->second.second)
                jt->on_component_stop(time_point);
        }
        it->second.second = false;
    }

    for(auto it = this->listeners.begin(); it != this->listeners.end(); it++)
    {
        for(auto&& jt : it->second.first)
        {
            if(!it->second.second)
                jt->on_component_start(time_point);
            jt->on_stream_start(time_point);
        }
        it->second.second = false;
    }
}

void media_message_generator::register_listener(
    const media_stream_message_listener_t& clock_sink, const media_component* key)
{
    scoped_lock lock(this->mutex_listeners);
    this->listeners[key].first.push_back(clock_sink);
    this->listeners[key].second = false;
}

void media_message_generator::clock_start(time_unit time_point)
{
    scoped_lock lock(this->mutex_listeners);
    for(auto it = this->listeners.begin(); it != this->listeners.end(); it++)
    {
        for(auto&& jt : it->second.first)
        {
            jt->on_component_start(time_point);
            jt->on_stream_start(time_point);
        }
        it->second.second = false;
    }
}

void media_message_generator::clock_stop(time_unit time_point)
{
    scoped_lock lock(this->mutex_listeners);
    for(auto it = this->listeners.begin(); it != this->listeners.end(); it++)
    {
        for(auto&& jt : it->second.first)
        {
            jt->on_stream_stop(time_point);
            jt->on_component_stop(time_point);
        }
        it->second.second = false;
    }
}

bool media_message_generator::is_drainable(time_unit t)
{
    scoped_lock lock(this->mutex_listeners);

    bool ret = true;
    for(auto it = this->listeners.begin(); it != this->listeners.end(); it++)
        for(auto&& jt : it->second.first)
            // currently, this must be called for every component for draining to work properly
            ret &= jt->is_drainable_or_drained(t);

    return ret;
}

void media_message_generator::clear_listeners()
{
    scoped_lock lock(this->mutex_listeners);
    this->listeners.clear();
}