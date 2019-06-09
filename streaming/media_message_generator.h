#pragma once
#include "media_sample.h"
#include <mutex>
#include <utility>
#include <map>
#include <vector>
#include <memory>

class media_component;
class media_stream_message_listener;
class media_clock;
class media_message_generator;
typedef std::shared_ptr<media_stream_message_listener> media_stream_message_listener_t;
typedef std::shared_ptr<media_clock> media_clock_t;
typedef std::shared_ptr<media_message_generator> media_message_generator_t;

class media_message_generator
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef std::map<const media_component*, std::pair<std::vector<media_stream_message_listener_t>, bool>>
        listeners_t;
private:
    listeners_t listeners;
    std::recursive_mutex mutex_listeners;

    void clock_start(time_unit time_point);
public:
    ~media_message_generator();

    void register_listener(const media_stream_message_listener_t&, const media_component*);

    // sends on_stream_stop(& on_component_stop) event using prev_event_generator and 
    // on_stream_start(& on_component_start) using this generator
    void clock_start(time_unit time_point, const media_message_generator_t& prev_event_generator);
    // notifies all event listeners
    void clock_stop(time_unit time_point);
    // returns whether the topology can generate samples up to the time point
    bool is_drainable(time_unit);

    void clear_listeners();
};