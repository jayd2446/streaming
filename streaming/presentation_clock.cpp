#include "presentation_clock.h"
#include <Mferror.h>
#include <cassert>
#include <limits>

extern LARGE_INTEGER pc_frequency;
typedef std::lock_guard<std::recursive_mutex> scoped_lock;

#ifdef max
#undef max
#endif


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


presentation_time_source::presentation_time_source() : running(false), current_time(0), time_off(0)
{
    this->start_time.QuadPart = 0;
}

time_unit presentation_time_source::performance_counter_to_time_unit(LARGE_INTEGER t2) const
{
    t2.QuadPart -= this->start_time.QuadPart;
    t2.QuadPart *= 1000000;
    t2.QuadPart /= pc_frequency.QuadPart;
    return t2.QuadPart * 10 + this->time_off;
}

time_unit presentation_time_source::system_time_to_time_source(time_unit t) const
{
    LARGE_INTEGER t2 = this->start_time;
    t2.QuadPart *= 1000000 * 10;
    t2.QuadPart /= pc_frequency.QuadPart;
    /*t2.QuadPart *= 10;*/

    return (t - t2.QuadPart) + this->time_off;
}

time_unit presentation_time_source::get_current_time() const
{
    if(!this->running)
        return this->current_time;

    // calculate the new current time
    LARGE_INTEGER current_time;
    QueryPerformanceCounter(&current_time);
    // this function automatically truncates
    this->current_time = this->performance_counter_to_time_unit(current_time);

    return this->current_time;
}

void presentation_time_source::set_current_time(time_unit t)
{
    this->current_time = t;

    // truncate to microsecond resolution
    this->current_time /= 10;
    this->time_off = (this->current_time *= 10);

    QueryPerformanceCounter(&this->start_time);
}

void presentation_time_source::start()
{
    assert(!this->running);
    this->set_current_time(this->get_current_time());
    this->running = true;
}

void presentation_time_source::stop()
{
    assert(this->running);
    this->running = false;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


DWORD wait_work_queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;

presentation_clock_sink::presentation_clock_sink() :
    wait_timer(CreateWaitableTimer(NULL, FALSE, NULL)),
    callback_key(0),
    scheduled_time(std::numeric_limits<time_unit>::max()),
    unregistered(true),
    fps_num(0),
    fps_den(0)
{
    this->callback.Attach(new async_callback_t(&presentation_clock_sink::callback_cb));
    // TODO: make the work queue static
    DWORD work_queue = wait_work_queue;
    if(!wait_work_queue)
        if(FAILED(MFAllocateWorkQueue(&work_queue)))
            throw std::exception();
    this->callback->native.work_queue = work_queue;
    /*if(!wait_work_queue)
        if(FAILED(MFBeginRegisterWorkQueueWithMMCSS(
            this->callback.work_queue, L"Capture", AVRT_PRIORITY_NORMAL, &this->callback2, NULL)))
            throw std::exception();*/
    wait_work_queue = work_queue;

    if(!this->wait_timer)
        throw std::exception();
}

presentation_clock_sink::~presentation_clock_sink()
{
    /*assert(this->unregistered);*/
    /*MFUnlockWorkQueue(this->callback.work_queue);*/
}

bool presentation_clock_sink::register_sink(presentation_clock_t& clock)
{
    assert(this->unregistered);

    scoped_lock lock(clock->mutex_sinks);
    clock->sinks.push_back(presentation_clock_sink_t(this->shared_from_this<presentation_clock_sink>()));

    this->unregistered = false;
    return true;
}

//bool presentation_clock_sink::unregister_sink()
//{
//    assert(!this->unregistered);
//
//    presentation_clock_t clock;
//    if(!this->get_clock(clock))
//        return false;
//
//    ::scoped_lock lock(clock->mutex_sinks);
//    for(auto it = clock->sinks.begin(); it != clock->sinks.end(); it++)
//    {
//        presentation_clock_sink_t sink((*it).lock());
//        if(!sink)
//        {
//            this->unregistered = true;
//            clock->sinks.erase(it);
//            return true;
//        }
//    }
//    return false;
//}

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
            HRESULT hr;

            if(FAILED(hr = MFCreateAsyncResult(NULL, &this->callback->native, NULL, &asyncresult)))
            {
                if(hr == MF_E_SHUTDOWN)
                    return true;
                throw std::exception();
            }
            if(FAILED(hr = this->callback->mf_put_waiting_work_item(
                this->shared_from_this<presentation_clock_sink>(),
                this->wait_timer, 0, asyncresult, &this->callback_key)))
            {
                if(hr == MF_E_SHUTDOWN)
                    return true;
                throw std::exception();
            }
        }
    }

    return true;
}

void presentation_clock_sink::callback_cb(void*)
{
    time_unit due_time;
    {
        scoped_lock lock(this->mutex_callbacks);
        this->callback_key = 0;
        // the callback might trigger even though the work item has been canceled
        // and the queue cleared
        if(this->callbacks.empty())
            return;

        assert(*this->callbacks.begin() == this->scheduled_time);

        due_time = *this->callbacks.begin();
        this->callbacks.erase(due_time);

        // schedule the next item
        this->scheduled_time = std::numeric_limits<time_unit>::max();
        while(!this->callbacks.empty() && !this->schedule_callback(*this->callbacks.begin()));
    }

    // invoke the callback
    this->scheduled_callback(due_time);
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

void presentation_clock_sink::scheduled_callback(time_unit)
{
    assert(false);
}

void presentation_clock_sink::set_pull_rate(int64_t fps_num, int64_t fps_den)
{
    this->fps_den = fps_den;
    this->fps_num = fps_num;
    this->fps_den_in_time_unit = this->fps_den * SECOND_IN_TIME_UNIT;
    this->pull_interval = this->fps_den_in_time_unit / fps_num + 1;
}

time_unit presentation_clock_sink::get_remainder(time_unit t) const
{
    assert(this->fps_num > 0);
    return ((t * this->fps_num) % this->fps_den_in_time_unit) / this->fps_num;
}

time_unit presentation_clock_sink::get_next_due_time(time_unit t) const
{
    t += this->pull_interval;
    t -= this->get_remainder(t);
    return t;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


presentation_clock::presentation_clock(const presentation_time_source_t& time_source) : 
    time_source(time_source)
{
}

presentation_clock::~presentation_clock()
{
    /*assert(this->sinks.empty());*/
}

bool presentation_clock::clock_start(time_unit time_point)
{
    bool stop_all = false;
    {
        scoped_lock lock(this->mutex_sinks);

        for(auto it = this->sinks.begin(); it != this->sinks.end(); it++)
        {
            if(!(*it)->on_clock_start(time_point))
            {
                stop_all = true;
                break;
            }
        }
    }

    if(stop_all)
        this->clock_stop(time_point);

    return !stop_all;
}

void presentation_clock::clock_stop(time_unit time_point)
{
    scoped_lock lock(this->mutex_sinks);
    for(auto it = this->sinks.begin(); it != this->sinks.end(); it++)
        (*it)->on_clock_stop(time_point);
}

void presentation_clock::clear_clock_sinks()
{
    scoped_lock lock(this->mutex_sinks);
    this->sinks.clear();
}