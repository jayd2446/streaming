#include "presentation_clock.h"
#include <cassert>

extern LARGE_INTEGER pc_frequency;
typedef std::lock_guard<std::mutex> scoped_lock;

presentation_clock_sink::presentation_clock_sink(presentation_clock_t& clock) :
    callback(this, &presentation_clock_sink::callback_cb),
    wait_timer(CreateWaitableTimer(NULL, FALSE, NULL)),
    callback_key(0),
    scheduled_time(0)
{
    if(!this->wait_timer)
        throw std::exception();

    ::scoped_lock lock(clock->mutex_sinks);
    clock->sinks.push_back(presentation_clock_sink_t(this));
}

bool presentation_clock_sink::schedule_callback()
{
    scoped_lock lock(this->mutex_callbacks);
    this->callback_key = 0;

    // return if there's no callbacks
    if(this->callbacks.empty())
    {
        this->scheduled_time = 0;
        return false;
    }

    const time_unit due_time = *this->callbacks.begin();
    // if the first item is late, simply drop it and try to schedule the next item
    const time_unit current_time = this->get_clock()->get_current_time();
    if(due_time <= current_time)
    {
        this->callbacks.erase(due_time);
        return this->schedule_callback();
    }

    // schedule the callback
    this->scheduled_time = due_time;
    LARGE_INTEGER due_time2;
    due_time2.QuadPart = current_time - due_time;
    if(SetWaitableTimer(this->wait_timer, &due_time2, 0, NULL, NULL, FALSE) == 0)
        throw std::exception();
    // queue a waititem to wait for timer completion
    CComPtr<IMFAsyncResult> asyncresult;
    if(FAILED(MFCreateAsyncResult(NULL, &this->callback, NULL, &asyncresult)))
        throw std::exception();
    if(FAILED(MFPutWaitingWorkItem(this->wait_timer, 0, asyncresult, &this->callback_key)))
        throw std::exception();

    return true;
}

HRESULT presentation_clock_sink::callback_cb(IMFAsyncResult*)
{
    time_unit due_time;
    {
        scoped_lock lock(this->mutex_callbacks);
        assert(!this->callbacks.empty());
        assert(*this->callbacks.begin() == this->scheduled_time);

        due_time = *this->callbacks.begin();
        this->callbacks.erase(due_time);
    }

    // schedule the next item
    // item removal in this routine is not necessary because schedule_callback will drop it
    // (seems like the wait timer signalling isn't quite exact)
    this->schedule_callback();

    // invoke the callback
    this->scheduled_callback(due_time);

    return S_OK;
}

void presentation_clock_sink::clear_queue()
{
    scoped_lock lock(this->mutex_callbacks);
    this->callbacks.clear();
    if(this->callback_key != 0)
        if(FAILED(MFCancelWorkItem(this->callback_key)))
            throw std::exception();

    this->callback_key = 0;
}

bool presentation_clock_sink::schedule_callback(time_unit t)
{
    const time_unit current_time = this->get_clock()->get_current_time();
    if(current_time >= t)
        return false;

    // TODO: maybe move this to a worker thread

    scoped_lock lock(this->mutex_callbacks);
    const bool reset_timer = this->callbacks.empty() || (*this->callbacks.begin() > t);
    this->callbacks.insert(t);

    // set the timer to signal for the first item after the timeout
    if(reset_timer)
        return this->schedule_callback();

    return true;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


presentation_clock::presentation_clock() : 
    running(false),
    current_time(0)
{
    this->start_time.QuadPart = 0;
}

time_unit presentation_clock::get_current_time() const
{
    if(!this->running)
        return this->current_time;

    // calculate the new current time
    LARGE_INTEGER current_time;
    QueryPerformanceCounter(&current_time);
    current_time.QuadPart -= this->start_time.QuadPart;
    current_time.QuadPart *= 1000000 * 10;
    current_time.QuadPart /= pc_frequency.QuadPart;
    this->current_time = current_time.QuadPart;

    return this->current_time;
}

bool presentation_clock::clock_start()
{
    bool stop_all = false;
    {
        scoped_lock lock(this->mutex_sinks);
        const time_unit t = this->get_current_time();

        for(auto it = this->sinks.begin(); it != this->sinks.end(); it++)
            if(!(*it)->on_clock_start(t))
            {
                stop_all = true;
                break;
            };
    }

    if(stop_all)
        this->clock_stop();
    else
    {
        QueryPerformanceCounter(&this->start_time);
        this->running = true;
    }

    return !stop_all;
}

void presentation_clock::clock_stop()
{
    scoped_lock lock(this->mutex_sinks);
    const time_unit t = this->get_current_time();
    for(auto it = this->sinks.begin(); it != this->sinks.end(); it++)
        (*it)->on_clock_stop(t);

    this->running = false;
}