#include "media_sample.h"
#include "assert.h"
#include <atomic>

media_buffer::media_buffer() : 
    read_lock(0),
    write_lock(false)
{
}

media_buffer::~media_buffer()
{
    assert_(!this->write_lock);
    assert_(!this->read_lock);
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


media_sample::media_sample(time_unit timestamp) : timestamp(timestamp)
{
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_texture::media_sample_texture(const media_buffer_texture_t& texture_buffer) :
    buffer(texture_buffer)
{
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_h264::media_sample_h264(const media_buffer_h264_t& buffer) :
    buffer(buffer)
{
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_audio::media_sample_audio(const media_buffer_samples_t& buffer) :
    buffer(buffer)
{
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_aac::media_sample_aac(const media_buffer_samples_t& buffer) :
    buffer(buffer)
{
}