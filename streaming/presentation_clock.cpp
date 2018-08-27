#include "presentation_clock.h"
#include <Mferror.h>
#include "assert.h"
#include <limits>

#ifdef max
#undef max
#endif


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


presentation_time_source::presentation_time_source() : 
    running(false), elapsed(time_unit_t::zero()), offset(time_unit_t::zero())
{
}

time_unit presentation_time_source::system_time_to_time_source(time_unit t) const
{
    using namespace std::chrono;
    typedef duration<double, time_unit_t::period> time_unit_conversion_t;
    typedef time_point<clock_t, time_unit_conversion_t> sys_time_point_t;
    scoped_lock lock(this->mutex);

    const sys_time_point_t sys_time = sys_time_point_t(time_unit_t(t));
    time_unit_conversion_t elapsed = 
        duration_cast<time_unit_conversion_t>(sys_time - this->start_time);
    elapsed += this->offset;

    return duration_cast<time_unit_t>(duration_cast<microseconds>(elapsed)).count();
}

time_unit presentation_time_source::get_current_time() const
{
    using namespace std::chrono;
    scoped_lock lock(this->mutex);

    if(this->running)
    {
        this->elapsed = duration_cast<time_unit_t>(this->clock.now() - this->start_time);
        this->elapsed += this->offset;
    }

    return duration_cast<time_unit_t>(duration_cast<microseconds>(this->elapsed)).count();
}

void presentation_time_source::set_current_time(time_unit t)
{
    using namespace std::chrono;
    scoped_lock lock(this->mutex);

    this->offset = time_unit_t(t);
    this->start_time = this->clock.now();
}

void presentation_time_source::start()
{
    assert_(!this->running);
    using namespace std::chrono;
    scoped_lock lock(this->mutex);

    this->start_time = this->clock.now();
    this->running = true;
}

void presentation_time_source::stop()
{
    assert_(this->running);
    scoped_lock lock(this->mutex);

    this->get_current_time();
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
    /*unregistered(true),*/
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
    /*assert_(this->unregistered);*/
    /*MFUnlockWorkQueue(this->callback.work_queue);*/
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
            HRESULT hr;

            if(FAILED(hr = MFCreateAsyncResult(NULL, &this->callback->native, NULL, &asyncresult)))
            {
                if(hr == MF_E_SHUTDOWN)
                    return true;
                throw std::exception();
            }
            if(FAILED(hr = this->callback->mf_put_waiting_work_item(
                this->shared_from_this<presentation_clock_sink>(),
                this->wait_timer, 10, asyncresult, &this->callback_key)))
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

        assert_(*this->callbacks.begin() == this->scheduled_time);

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
    scoped_lock lock(this->mutex_callbacks);
    this->callbacks.insert(t);

    return this->schedule_callback(t);
}

void presentation_clock_sink::scheduled_callback(time_unit)
{
    assert_(false);
}

void presentation_clock_sink::set_pull_rate(int64_t fps_num, int64_t fps_den)
{
    this->fps_den = fps_den;
    this->fps_num = fps_num;
    this->fps_den_in_time_unit = this->fps_den * SECOND_IN_TIME_UNIT;
    this->pull_interval = this->fps_den_in_time_unit / this->fps_num + 1;
}

time_unit presentation_clock_sink::get_remainder(time_unit t) const
{
    assert_(this->fps_num > 0);
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
    /*assert_(this->sinks.empty());*/
}

void presentation_clock::clock_start(time_unit time_point, const presentation_clock_t& prev_clock)
{
    if(!prev_clock)
    {
        this->clock_start(time_point);
        return;
    }

    assert_(this->get_time_source().get() == prev_clock->get_time_source().get());

    scoped_lock lock(this->mutex_sinks), lock2(prev_clock->mutex_sinks);
    for(auto it = prev_clock->sinks.begin(); it != prev_clock->sinks.end(); it++)
    {
        for(auto jt = this->sinks.begin(); jt != this->sinks.end(); jt++)
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

    for(auto it = this->sinks.begin(); it != this->sinks.end(); it++)
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

void presentation_clock::register_sink(
    const media_stream_clock_sink_t& clock_sink, const media_component* key)
{
    scoped_lock lock(this->mutex_sinks);
    this->sinks[key].first.push_back(clock_sink);
    this->sinks[key].second = false;
    
}

void presentation_clock::clock_start(time_unit time_point)
{
    scoped_lock lock(this->mutex_sinks);
    for(auto it = this->sinks.begin(); it != this->sinks.end(); it++)
    {
        for(auto&& jt : it->second.first)
        {
            jt->on_component_start(time_point);
            jt->on_stream_start(time_point);
        }
        it->second.second = false;
    }
}

void presentation_clock::clock_stop(time_unit time_point)
{
    scoped_lock lock(this->mutex_sinks);
    for(auto it = this->sinks.begin(); it != this->sinks.end(); it++)
    {
        for(auto&& jt : it->second.first)
        {
            jt->on_stream_stop(time_point);
            jt->on_component_stop(time_point);
        }
        it->second.second = false;
    }
}

void presentation_clock::clear_clock_sinks()
{
    scoped_lock lock(this->mutex_sinks);
    this->sinks.clear();
}