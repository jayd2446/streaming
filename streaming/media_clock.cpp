#include "media_clock.h"
#include "media_stream.h"
#include "assert.h"
#include <limits>

media_clock::media_clock() : 
    running(false), elapsed(time_unit_t::zero()), offset(time_unit_t::zero())
{
}

time_unit media_clock::system_time_to_clock_time(LONGLONG t) const
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

time_unit media_clock::get_current_time() const
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

void media_clock::set_current_time(time_unit t)
{
    using namespace std::chrono;
    scoped_lock lock(this->mutex);

    this->offset = time_unit_t(t);
    this->start_time = this->clock.now();
}

void media_clock::start()
{
    assert_(!this->running);
    using namespace std::chrono;
    scoped_lock lock(this->mutex);

    this->start_time = this->clock.now();
    this->running = true;
}

void media_clock::stop()
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

media_clock_sink::media_clock_sink() :
    wait_timer(CreateWaitableTimer(NULL, FALSE, NULL)),
    callback_key(0),
    scheduled_time(std::numeric_limits<time_unit>::max()),
    /*unregistered(true),*/
    fps_num(0),
    fps_den(0)
{
    HRESULT hr = S_OK;

    this->callback.Attach(new async_callback_t(&media_clock_sink::callback_cb));
    // TODO: make the work queue static
    DWORD work_queue = wait_work_queue;
    if(!wait_work_queue)
        if(FAILED(hr = MFAllocateWorkQueue(&work_queue)))
            throw HR_EXCEPTION(hr);
    this->callback->native.work_queue = work_queue;
    /*if(!wait_work_queue)
        if(FAILED(MFBeginRegisterWorkQueueWithMMCSS(
            this->callback.work_queue, L"Capture", AVRT_PRIORITY_NORMAL, &this->callback2, NULL)))
            throw HR_EXCEPTION(hr);*/
    wait_work_queue = work_queue;

    if(!this->wait_timer)
        throw HR_EXCEPTION(hr = E_UNEXPECTED);
}

media_clock_sink::~media_clock_sink()
{
    /*assert_(this->unregistered);*/
    /*MFUnlockWorkQueue(this->callback.work_queue);*/
}

void media_clock_sink::callback_cb(void*)
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

        // TODO: drop multi callback queue
        // schedule the next item
        this->scheduled_time = std::numeric_limits<time_unit>::max();
        assert_(this->callbacks.empty());
        /*while(!this->callbacks.empty() && !this->schedule_callback(*this->callbacks.begin()));*/
    }

    // invoke the callback
    this->scheduled_callback(due_time);
}

bool media_clock_sink::clear_queue()
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

void media_clock_sink::scheduled_callback(time_unit)
{
    assert_(false);
}

void media_clock_sink::set_pull_rate(int64_t fps_num, int64_t fps_den)
{
    this->fps_den = fps_den;
    this->fps_num = fps_num;
    this->fps_den_in_time_unit = this->fps_den * SECOND_IN_TIME_UNIT;
    this->pull_interval = this->fps_den_in_time_unit / this->fps_num + 1;
}

time_unit media_clock_sink::get_remainder(time_unit t) const
{
    assert_(this->fps_num > 0);
    return ((t * this->fps_num) % this->fps_den_in_time_unit) / this->fps_num;
}

time_unit media_clock_sink::get_next_due_time(time_unit t) const
{
    t += this->pull_interval;
    t -= this->get_remainder(t);
    return t;
}