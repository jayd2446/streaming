#pragma once
#include "async_callback.h"
#include "media_sample.h"
#include "enable_shared_from_this.h"
#include <map>
#include <set>
#include <vector>
#include <mutex>
#include <chrono>
#include <mfapi.h>

// times are truncated to microsecond resolution
class presentation_time_source
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef std::chrono::high_resolution_clock clock_t;
    typedef std::chrono::duration<time_unit, std::ratio<100, 1000000000>> time_unit_t;
private:
    bool running;
    mutable std::recursive_mutex mutex;

    clock_t clock;
    mutable clock_t::time_point start_time;
    mutable time_unit_t elapsed, offset;
public:
    presentation_time_source();

    time_unit system_time_to_time_source(time_unit) const;

    time_unit get_current_time() const;
    void set_current_time(time_unit);

    bool is_running() const {return this->running;}
    // begins incrementing the current time
    void start();
    void stop();
};

typedef std::shared_ptr<presentation_time_source> presentation_time_source_t;

class presentation_clock;
typedef std::shared_ptr<presentation_clock> presentation_clock_t;

// TODO: scheduling assumes clock increments based on real time(the time source should 
// implement scheduling)
// TODO: rename to scheduling
class presentation_clock_sink : public virtual enable_shared_from_this
{
    friend class presentation_clock;
public:
    typedef std::set<time_unit> sorted_set_t;
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<presentation_clock_sink> async_callback_t;
private:
    /*bool unregistered;*/
    CComPtr<async_callback_t> callback;
    sorted_set_t callbacks;
    std::recursive_mutex mutex_callbacks;
    CHandle wait_timer;
    MFWORKITEM_KEY callback_key;
    time_unit scheduled_time;

    time_unit pull_interval;
    time_unit fps_num, fps_den;
    time_unit fps_den_in_time_unit;
    time_unit get_remainder(time_unit t) const;

    // schedules the next callback in the list
    bool schedule_callback(time_unit due_time);
    void callback_cb(void*);
protected:
    // returns false if mfcancelworkitem fails
    bool clear_queue();

    // returns false if the time has already passed;
    // adds a new callback time to the list
    bool schedule_new_callback(time_unit due_time);

    // a new callback shouldn't be scheduled if the topology isn't active anymore
    virtual void scheduled_callback(time_unit due_time);
public:
    presentation_clock_sink();
    virtual ~presentation_clock_sink();

    void set_pull_rate(int64_t fps_num, int64_t fps_den);
    time_unit get_pull_interval() const {return this->pull_interval;}
    time_unit get_next_due_time(time_unit) const;

    void set_schedule_cb_work_queue(DWORD w) {this->callback->native.work_queue = w;}

    // can be NULL;
    // get_clock must be an atomic operation
    virtual bool get_clock(presentation_clock_t&) = 0;
};

typedef std::shared_ptr<presentation_clock_sink> presentation_clock_sink_t;

class media_component;
class media_stream;
class media_stream_clock_sink;
typedef std::shared_ptr<media_stream> media_stream_t;
typedef std::shared_ptr<media_stream_clock_sink> media_stream_clock_sink_t;

class presentation_clock
{
    friend class presentation_clock_sink;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef std::map<const media_component*, std::pair<std::vector<media_stream_clock_sink_t>, bool>> 
        sinks_t;
private:
    sinks_t sinks;
    std::recursive_mutex mutex_sinks;
    presentation_time_source_t time_source;

    void clock_start(time_unit time_point);
public:
    explicit presentation_clock(const presentation_time_source_t&);
    ~presentation_clock();

    const presentation_time_source_t& get_time_source() const {return this->time_source;}
    time_unit get_current_time() const {return this->time_source->get_current_time();}

    void register_sink(const media_stream_clock_sink_t&, const media_component*);

    // stops the old clock and starts this clock;
    // notifies streams whether the component is stopped or started;
    // it is assumed that both clocks use the same time source
    void clock_start(time_unit time_point, const presentation_clock_t& prev_clock);
    // notifies all sinks
    void clock_stop(time_unit time_point);
    // calls sinks with the current time source's time as the time point parameter
    /*void clock_stop() {this->clock_stop(this->get_current_time());}*/

    void clear_clock_sinks();
};