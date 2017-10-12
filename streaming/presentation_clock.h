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

// TODO: time unit should probably be defined here

class presentation_clock;
typedef std::shared_ptr<presentation_clock> presentation_clock_t;

void ticks_to_time_unit(LARGE_INTEGER&);

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
    virtual bool on_clock_start(time_unit, int packet_number) = 0;
    virtual void on_clock_stop(time_unit) = 0;

    // a new callback shouldn't be scheduled if the topology isn't active anymore
    virtual void scheduled_callback(time_unit due_time) = 0;
public:
    presentation_clock_sink();
    virtual ~presentation_clock_sink();

    // adds this sink to the list of sinks in the presentation clock
    bool register_sink(presentation_clock_t&);
    // (not needed when using shared ptrs instead of weak ptrs)
    //// returns false if clock wasn't found or sink wasn't found in clock;
    //// this actually removes one invalid pointer from the sink list
    //// TODO: rename this to something more fitting
    //bool unregister_sink();

    // can be NULL;
    // get_clock must be an atomic operation
    virtual bool get_clock(presentation_clock_t&) = 0;
};

typedef std::shared_ptr<presentation_clock_sink> presentation_clock_sink_t;

// TODO: multithreading safety needs to be ensured
class presentation_clock
{
    friend class presentation_clock_sink;
public:
    // if clock is relocated to session, this vector should include weak ptrs
    // to avoid circular dependency
    typedef std::vector<presentation_clock_sink_t> vector_t;
private:
    vector_t sinks;
    std::recursive_mutex mutex_sinks;

    time_unit time_off;
    LARGE_INTEGER start_time;
    mutable LARGE_INTEGER current_time_ticks;
    mutable time_unit current_time;
    bool running;
public:
    presentation_clock();
    ~presentation_clock();

    // returns the internal time which is used to calculate the current time
    LARGE_INTEGER get_start_time_internal() const {return this->start_time;}

    time_unit get_current_time() const;
    void set_current_time(time_unit t);

    bool clock_running() const {return this->running;}
    // calls clock_stop if one of the sinks returned false
    // and returns false;
    // starts the timer and calls sinks with the supplied time parameter;
    // by default also sets the time to time parameter
    bool clock_start(time_unit time, bool set_time = true, int packet_number = 0);
    // returns false if any of the sinks returns false;
    // stops the timer and calls sinks with the supplied time parameter;
    // by default also sets the time to time parameter
    void clock_stop(time_unit time, bool set_time = true);
    void clock_stop() {this->clock_stop(this->get_current_time());}

    void clear_clock_sinks();
};