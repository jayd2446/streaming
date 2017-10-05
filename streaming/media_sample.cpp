#include "media_sample.h"

media_sample::media_sample() : available(true), frame(NULL)
{
}

bool media_sample::try_lock_sample()
{
    std::unique_lock<std::mutex> lock(this->mutex);
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
    std::unique_lock<std::mutex> lock(this->mutex);
    while(!this->available)
        // wait unlocks the mutex and reacquires the lock when it is notified
        this->cv.wait(lock);
    this->available = false;
    lock.unlock();
}

void media_sample::unlock_sample()
{
    std::lock_guard<std::mutex> lock(this->mutex);
    this->available = true;
    this->cv.notify_all();
}