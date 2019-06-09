#pragma once
#include "async_callback.h"
#include "media_sample.h"
#include "media_message_generator.h"
#include "enable_shared_from_this.h"
#include <map>
#include <set>
#include <vector>
#include <mutex>
#include <chrono>
#include <mfapi.h>
#include <Mferror.h>

// times are truncated to microsecond resolution
class media_clock
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
    media_clock();

    time_unit system_time_to_clock_time(LONGLONG) const;

    time_unit get_current_time() const;
    void set_current_time(time_unit);

    bool is_running() const {return this->running;}
    // begins incrementing the current time
    void start();
    void stop();
};

typedef std::shared_ptr<media_clock> media_clock_t;

// TODO: scheduling assumes clock increments based on real time(the time source should 
// implement scheduling)
// TODO: rename to scheduling
// derived class must inherit from enable_shared_from_this
class media_clock_sink
{
    friend class media_message_generator;
public:
    typedef std::set<time_unit> sorted_set_t;
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<media_clock_sink> async_callback_t;
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
    template<typename Derived>
    bool schedule_callback(time_unit due_time);
    void callback_cb(void*);
protected:
    // returns false if mfcancelworkitem fails
    bool clear_queue();

    // returns false if the time has already passed;
    // adds a new callback time to the list
    template<typename Derived>
    bool schedule_new_callback(time_unit due_time);

    // a new callback shouldn't be scheduled if the topology isn't active anymore
    virtual void scheduled_callback(time_unit due_time);
public:
    media_clock_sink();
    virtual ~media_clock_sink();

    void set_pull_rate(int64_t fps_num, int64_t fps_den);
    time_unit get_pull_interval() const {return this->pull_interval;}
    time_unit get_next_due_time(time_unit) const;

    void set_schedule_cb_work_queue(DWORD w) {this->callback->native.work_queue = w;}

    // can be NULL;
    // get_clock must be an atomic operation
    virtual bool get_clock(media_clock_t&) = 0;
};

typedef std::shared_ptr<media_clock_sink> presentation_clock_sink_t;


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<typename T>
bool media_clock_sink::schedule_callback(time_unit due_time)
{
    scoped_lock lock(this->mutex_callbacks);
    HRESULT hr = S_OK;

    // return if there's no callbacks
    if(this->callbacks.empty())
    {
        this->scheduled_time = std::numeric_limits<time_unit>::max();
        return false;
    }

    // clear the queue if there's no clock available anymore
    media_clock_t clock;
    if(!this->get_clock(clock))
    {
        this->clear_queue();
        return false;
    }
    // drop the callback if the time has already passed
    const time_unit current_time = clock->get_current_time();
    if(due_time <= current_time)
    {
        this->callbacks.erase(due_time);
        return false;
    }

    // schedule the callback
    if(due_time < this->scheduled_time)
    {
        this->scheduled_time = due_time;
        LARGE_INTEGER due_time2;
        due_time2.QuadPart = current_time - due_time;
        if(SetWaitableTimer(this->wait_timer, &due_time2, 0, NULL, NULL, FALSE) == 0)
            throw HR_EXCEPTION(hr = E_UNEXPECTED);

        // create a new waiting work item if the old one expired
        if(this->callback_key == 0)
        {
            // queue a waititem to wait for timer completion
            CComPtr<IMFAsyncResult> asyncresult;
            HRESULT hr;

            // TODO: create the async result in function constructor
            if(FAILED(hr = MFCreateAsyncResult(NULL, &this->callback->native, NULL, &asyncresult)))
            {
                if(hr == MF_E_SHUTDOWN)
                    return true;
                throw HR_EXCEPTION(hr);
            }
            if(FAILED(hr = this->callback->mf_put_waiting_work_item(
                static_cast<T*>(this)->shared_from_this<T>(),
                this->wait_timer, 0, asyncresult, &this->callback_key)))
            {
                if(hr == MF_E_SHUTDOWN)
                    return true;
                throw HR_EXCEPTION(hr);
            }
        }
    }

    return true;
}

template<typename T>
bool media_clock_sink::schedule_new_callback(time_unit t)
{
    scoped_lock lock(this->mutex_callbacks);
    this->callbacks.insert(t);

    return this->schedule_callback<T>(t);
}