#include "media_sample.h"

media_sample::media_sample() : available(true), frame(NULL)
{
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
    this->cv.notify_all();
}

media_sample_view::media_sample_view(const media_sample_t& sample) :
    sample(sample)
{
    this->sample->lock_sample();
}

media_sample_view::~media_sample_view()
{
    this->sample->unlock_sample();
}