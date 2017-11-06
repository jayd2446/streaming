#include "media_sample.h"
#include "assert.h"
#include <atomic>

media_buffer::media_buffer() : 
    read_lock(0),
    write_lock(false)
{
}

void media_buffer::lock()
{
    scoped_lock lock(this->mutex);
    while(this->read_lock > 0 || this->write_lock)
        // wait unlocks the mutex and reacquires the lock when it is notified
        this->cv.wait(lock);
    this->read_lock = 1;
    this->write_lock = true;
}

void media_buffer::lock_read()
{
    scoped_lock lock(this->mutex);
    while(this->write_lock)
        this->cv.wait(lock);
    this->read_lock++;
}

void media_buffer::unlock_write()
{
    scoped_lock lock(this->mutex);
    assert_(this->write_lock);
    assert_(this->read_lock > 0);
    this->write_lock = false;
    this->cv.notify_all();
}

void media_buffer::unlock()
{
    scoped_lock lock(this->mutex);
    /*assert_(this->read_lock);*/
    this->write_lock = false;
    this->read_lock--;
    assert_(this->read_lock >= 0);
    this->cv.notify_all();
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_view::media_sample_view(const media_sample_t& sample, view_lock_t view_lock) :
    sample(sample),
    view_lock(view_lock)
{
    if(this->view_lock == view_lock_t::READ_LOCK_BUFFERS)
        this->sample->buffer->lock_read();
    else if(this->view_lock == view_lock_t::LOCK_BUFFERS)
        this->sample->buffer->lock();
    else
        assert_(false);
}

media_sample_view::~media_sample_view()
{
    this->sample->buffer->unlock();
}