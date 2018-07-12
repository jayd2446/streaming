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


media_sample::media_sample(const media_buffer_t& buffer, time_unit timestamp) :
    buffer(buffer), timestamp(timestamp)
{
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_::media_sample_(time_unit timestamp) : timestamp(timestamp)
{
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_texture_::media_sample_texture_(const media_buffer_texture_t& texture_buffer) :
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


media_sample_view::media_sample_view(const media_buffer_t& buffer, view_lock_t view_lock) :
    sample(buffer)
{
    if(view_lock == view_lock_t::READ_LOCK_BUFFERS)
        this->sample.buffer->lock_read();
    else if(view_lock == view_lock_t::LOCK_BUFFERS)
        this->sample.buffer->lock();
    else
        assert_(false);
}

media_sample_view::~media_sample_view()
{
    if(this->sample.buffer)
        this->sample.buffer->unlock();
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_view_texture::media_sample_view_texture(
    const media_buffer_texture_t& texture, view_lock_t view_lock) :
    texture_buffer(texture),
    media_sample_view(texture, view_lock)
{
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


void media_sample_view_base::deleter(media_buffer* buffer)
{
    if(buffer)
        buffer->unlock();
}


//media_sample_view_::~media_sample_view_()
//{
//    this->detach();
//}
//
//void media_sample_view_::lock(view_lock_t view_lock)
//{
//     if(view_lock == READ_LOCK_BUFFERS)
//        this->buffer_view->lock_read();
//    else if(view_lock == LOCK_BUFFERS)
//        this->buffer_view->lock();
//    else
//        assert_(false);
//}
//
//void media_sample_view_::detach()
//{
//    this->buffer_view.reset();
//    this->buffer.reset();
//}
//
//void media_sample_view_::deleter(media_buffer* buffer)
//{
//    buffer->unlock();
//}