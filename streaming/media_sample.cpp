#include "media_sample.h"
#include <cassert>

media_sample::media_sample() : 
    available(true),
    read_lock(false),
    write_lock(false)
{
}

void media_sample::lock_sample_()
{
    scoped_lock lock(this->mutex);
    while(this->read_lock && this->write_lock)
        // wait unlocks the mutex and reacquires the lock when it is notified
        this->cv.wait(lock);
    this->read_lock = this->write_lock = true;
}

void media_sample::read_lock_sample()
{
    scoped_lock lock(this->mutex);
    while(this->write_lock)
        this->cv.wait(lock);
    this->read_lock = true;
}

void media_sample::unlock_write_lock_sample()
{
    scoped_lock lock(this->mutex);
    assert(this->write_lock);
    assert(this->read_lock);
    this->write_lock = false;
    this->cv.notify_all();
}

bool media_sample::try_lock_sample()
{
    scoped_lock lock(this->mutex);
    if(this->available)
    {
        this->available = false;
        return true;
    }
    else
        return false;
}

void media_sample::lock_sample()
{
    scoped_lock lock(this->mutex);
    while(!this->available)
        // wait unlocks the mutex and reacquires the lock when it is notified
        this->cv.wait(lock);
    this->available = false;
}

void media_sample::unlock_sample()
{
    scoped_lock lock(this->mutex);
    this->available = true;
    this->read_lock = this->write_lock = false;
    this->cv.notify_all();
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_texture::media_sample_texture() : shared_handle(NULL)
{
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////

media_sample_view::media_sample_view(const media_sample_t& sample, view_lock_t view_lock) :
    sample(sample),
    view_lock(view_lock)
{
    if(this->view_lock == view_lock_t::READ_LOCK_SAMPLE)
        this->sample->read_lock_sample();
    else if(this->view_lock == view_lock_t::LOCK_SAMPLE)
        this->sample->lock_sample_();
    else
        assert(false);
}

media_sample_view::media_sample_view(const media_sample_t& sample, bool already_locked) :
    sample(sample)
{
    if(!already_locked)
        this->sample->lock_sample();
}

media_sample_view::~media_sample_view()
{
    this->sample->unlock_sample();
}