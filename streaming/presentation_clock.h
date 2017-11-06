#pragma once
#include "async_callback.h"
#include "media_sample.h"
#include "enable_shared_from_this.h"
#include <map>
#include <set>
#include <vector>
#include <mutex>
#include <mfapi.h>
#include <atlbase.h>

// implements time that is based on high frequency clock;
// source times must be restricted to microsecond resolution
// TODO: multithreading safety needs to be ensured;
class presentation_time_source
{
private:
    time_unit time_off;
    LARGE_INTEGER start_time;
    mutable time_unit current_time;
    bool running;

    time_unit performance_counter_to_time_unit(LARGE_INTEGER) const;
public:
    presentation_time_source();

    // TODO: the calculations in this function might overflow
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
// TODO: decouple this class to sink and scheduling
class presentation_clock_sink : public virtual enable_shared_from_this
{
    friend class presentation_clock;
public:
    typedef std::set<time_unit> sorted_set_t;
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<presentation_clock_sink> async_callback_t;
private:
    bool unregistered;
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

    // called from media session's start and stop functions
    virtual bool on_clock_start(time_unit) = 0;
    virtual void on_clock_stop(time_unit) = 0;

    // a new callback shouldn't be scheduled if the topology isn't active anymore
    virtual void scheduled_callback(time_unit due_time);
public:
    presentation_clock_sink();
    virtual ~presentation_clock_sink();

    void set_pull_rate(int64_t fps_num, int64_t fps_den);
    time_unit get_pull_interval() const {return this->pull_interval;}
    time_unit get_next_due_time(time_unit) const;

    // adds this sink to the list of sinks in the presentation clock
    bool register_sink(presentation_clock_t&);
    // (not needed when using shared ptrs instead of weak ptrs)
    //// returns false if clock wasn't found or sink wasn't found in clock;
    //// this actually removes one invalid pointer from the sink list
    //// TODO: rename this to something more fitting
    //bool unregister_sink();

    void set_schedule_cb_work_queue(DWORD w) {this->callback->native.work_queue = w;}

    // can be NULL;
    // get_clock must be an atomic operation
    virtual bool get_clock(presentation_clock_t&) = 0;
};

typedef std::shared_ptr<presentation_clock_sink> presentation_clock_sink_t;

class presentation_clock
{
    friend class presentation_clock_sink;
public:
    typedef std::vector<presentation_clock_sink_t> vector_t;
private:
    vector_t sinks;
    std::recursive_mutex mutex_sinks;
    presentation_time_source_t time_source;
public:
    explicit presentation_clock(const presentation_time_source_t&);
    ~presentation_clock();

    const presentation_time_source_t& get_time_source() const {return this->time_source;}
    time_unit get_current_time() const {return this->time_source->get_current_time();}

    // calls clock_stop if one of the sinks returned false
    // and returns false;
    // calls sink with the time point parameter
    bool clock_start(time_unit time_point);
    // returns false if any of the sinks returns false;
    // calls sink with the time point parameter
    void clock_stop(time_unit time_point);
    // calls sinks with the current time source's time as the time point parameter
    void clock_stop() {this->clock_stop(this->get_current_time());}

    void clear_clock_sinks();
};