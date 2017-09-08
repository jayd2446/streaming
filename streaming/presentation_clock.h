#pragma once
#include "IUnknownImpl.h"
#include "AsyncCallback.h"
#include "media_sample.h"
#include <memory>
#include <map>
#include <set>
#include <vector>
#include <mutex>
#include <mfapi.h>
#include <atlbase.h>

// TODO: time unit should probably be defined here

class presentation_clock;
typedef std::shared_ptr<presentation_clock> presentation_clock_t;

// TODO: scheduled callback should be canceled if the clock stops
class presentation_clock_sink : public virtual IUnknownImpl
{
    friend class presentation_clock;
public:
    typedef std::set<time_unit> sorted_set_t;
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    AsyncCallback<presentation_clock_sink> callback;
    sorted_set_t callbacks;
    std::recursive_mutex mutex_callbacks;
    //CHandle wait_timer;
    MFWORKITEM_KEY callback_key;
    time_unit scheduled_time;

    // schedules the next callback in the list
    bool schedule_callback();
    HRESULT callback_cb(IMFAsyncResult*);
protected:
    void clear_queue();

    CHandle wait_timer;

    // returns false if the time has already passed;
    // adds a new callback time to the list
    bool schedule_callback(time_unit due_time);

    // called from media session's start and stop functions
    virtual bool on_clock_start(time_unit) = 0;
    virtual void on_clock_stop(time_unit) = 0;

    // a new callback shouldn't be scheduled if the topology isn't active anymore
    virtual void scheduled_callback(time_unit due_time) = 0;
public:
    // adds this sink to the list of sinks in the presentation clock
    explicit presentation_clock_sink(presentation_clock_t&);
    virtual ~presentation_clock_sink() {}

    virtual presentation_clock_t get_clock() = 0;
};

typedef CComPtr<presentation_clock_sink> presentation_clock_sink_t;

class presentation_clock
{
    friend class presentation_clock_sink;
public:
    typedef std::vector<presentation_clock_sink_t> vector_t;
private:
    vector_t sinks;
    std::mutex mutex_sinks;

    LARGE_INTEGER start_time;
    mutable time_unit current_time;
    bool running;
public:
    presentation_clock();

    time_unit get_current_time() const;
    void set_current_time(time_unit t) {this->current_time = t;}

    bool clock_running() const {return this->running;}
    // calls clock_stop if one of the sinks returned false
    // and returns false;
    // starts the timer aswell
    bool clock_start();
    // returns false if any of the sinks returns false;
    // stops the timer
    void clock_stop();
};