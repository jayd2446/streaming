#include "presentation_clock.h"
#include <cassert>
#include <limits>

extern LARGE_INTEGER pc_frequency;
typedef std::lock_guard<std::mutex> scoped_lock;

#ifdef max
#undef max
#endif

void ticks_to_time_unit(LARGE_INTEGER& ticks)
{
    ticks.QuadPart *= 1000000 * 10;
    ticks.QuadPart /= pc_frequency.QuadPart;
}

presentation_clock_sink::presentation_clock_sink(presentation_clock_t& clock) :
    callback(this, &presentation_clock_sink::callback_cb),
    wait_timer(CreateWaitableTimer(NULL, FALSE, NULL)),
    callback_key(0),
    scheduled_time(std::numeric_limits<time_unit>::max())
{
    if(!this->wait_timer)
        throw std::exception();

    ::scoped_lock lock(clock->mutex_sinks);
    clock->sinks.push_back(presentation_clock_sink_t(this));
}

bool presentation_clock_sink::schedule_callback(time_unit due_time)
{
    scoped_lock lock(this->mutex_callbacks);

    // return if there's no callbacks
    if(this->callbacks.empty())
    {
        this->scheduled_time = std::numeric_limits<time_unit>::max();
        return false;
    }

    // clear the queue if there's no clock available anymore
    presentation_clock_t clock;
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
            throw std::exception();

        // create a new waiting work item if the old one expired
        if(this->callback_key == 0)
        {
            // queue a waititem to wait for timer completion
            CComPtr<IMFAsyncResult> asyncresult;
            // mfputwaitingworkitem 
            if(FAILED(MFCreateAsyncResult(NULL, &this->callback, NULL, &asyncresult)))
                throw std::exception();
            if(FAILED(MFPutWaitingWorkItem(this->wait_timer, 0, asyncresult, &this->callback_key)))
                throw std::exception();
        }
    }

    return true;
}

HRESULT presentation_clock_sink::callback_cb(IMFAsyncResult*)
{
    time_unit due_time;
    {
        scoped_lock lock(this->mutex_callbacks);
        this->callback_key = 0;
        // the callback might trigger even though the work item has been canceled
        // and the queue cleared
        if(this->callbacks.empty())
            return S_OK;

        assert(*this->callbacks.begin() == this->scheduled_time);

        due_time = *this->callbacks.begin();
        this->callbacks.erase(due_time);

        // schedule the next item
        this->scheduled_time = std::numeric_limits<time_unit>::max();
        while(!this->callbacks.empty() && !this->schedule_callback(*this->callbacks.begin()));
    }

    // invoke the callback
    this->scheduled_callback(due_time);

    return S_OK;
}

bool presentation_clock_sink::clear_queue()
{
    scoped_lock lock(this->mutex_callbacks);

    this->callbacks.clear();
    this->scheduled_time = std::numeric_limits<time_unit>::max();

    //if(this->callback_key != 0)
    //    if(FAILED(MFCancelWorkItem(this->callback_key)))
    //    {
    //        /*this->callback_key = 0;*/
    //        return false;
    //    }

    //this->callback_key = 0;
    return true;
}

bool presentation_clock_sink::schedule_new_callback(time_unit t)
{
    // TODO: maybe move this to a worker thread
    scoped_lock lock(this->mutex_callbacks);
    this->callbacks.insert(t);

    return this->schedule_callback(t);
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


presentation_clock::presentation_clock() : 
    running(false),
    current_time(0)
{
    this->current_time_ticks.QuadPart = this->start_time.QuadPart = 0;
}

presentation_clock::~presentation_clock()
{
}

time_unit presentation_clock::get_current_time() const
{
    if(!this->running)
        return this->current_time;

    // calculate the new current time
    LARGE_INTEGER current_time;
    QueryPerformanceCounter(&current_time);
    current_time.QuadPart -= this->start_time.QuadPart;
    this->current_time_ticks.QuadPart = current_time.QuadPart;
    current_time.QuadPart *= 1000000 * 10;
    current_time.QuadPart /= pc_frequency.QuadPart;
    this->current_time = current_time.QuadPart;

    return this->current_time;
}

bool presentation_clock::clock_start(time_unit start_time)
{
    bool stop_all = false;
    {
        scoped_lock lock(this->mutex_sinks);
        this->set_current_time(start_time);
        const time_unit t = start_time;

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
        this->start_time.QuadPart -= this->current_time_ticks.QuadPart;
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

void presentation_clock::clear_clock_sinks()
{
    scoped_lock lock(this->mutex_sinks);
    this->sinks.clear();
}