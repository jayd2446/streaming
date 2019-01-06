#include "media_sample.h"
#include "assert.h"
#include <cmath>
#include <atomic>
#include <limits>

frame_unit convert_to_frame_unit(time_unit t, frame_unit frame_rate_num, frame_unit frame_rate_den)
{
    assert_(frame_rate_num >= 0);
    assert_(frame_rate_den > 0);

    const double frame_duration = SECOND_IN_TIME_UNIT / ((double)frame_rate_num / frame_rate_den);
    return (frame_unit)std::round(t / frame_duration);
}

time_unit convert_to_time_unit(frame_unit pos, frame_unit frame_rate_num, frame_unit frame_rate_den)
{
    assert_(frame_rate_num >= 0);
    assert_(frame_rate_den > 0);

    const double frame_duration = SECOND_IN_TIME_UNIT / ((double)frame_rate_num / frame_rate_den);
    return (frame_unit)std::round(pos * frame_duration);
}

template<typename Buffer>
class media_buffer_lockable : public media_buffer_trackable<Buffer>
{
private:
    volatile int read_lock;
    volatile bool write_lock;
    std::condition_variable cv;
    mutable std::mutex mutex;

    // waits for write and read to complete
    void lock();
    // waits only for the write to complete
    void lock_read();
    // unlocks the write and read lock
    void unlock();

    // unlocks the Buffer
    void on_delete();
public:
    media_buffer_lockable();
    virtual ~media_buffer_lockable();

    // creates the trackable buffer
    buffer_t lock_buffer(buffer_lock_t = buffer_lock_t::LOCK);

    // unlocks the write lock
    void unlock_write();

    bool is_locked() const;
};

template<typename T>
media_buffer_lockable<T>::media_buffer_lockable() :
    read_lock(0),
    write_lock(false)
{
}

template<typename T>
media_buffer_lockable<T>::~media_buffer_lockable()
{
    assert_(!this->is_locked());
}

template<typename T>
void media_buffer_lockable<T>::on_delete()
{
    this->unlock();
}

template<typename T>
typename media_buffer_lockable<T>::buffer_t
media_buffer_lockable<T>::lock_buffer(buffer_lock_t lock_t)
{
    if(lock_t == buffer_lock_t::LOCK)
        this->lock();
    else
        this->lock_read();

    return this->create_tracked_buffer();
}

template<typename T>
void media_buffer_lockable<T>::lock()
{
    scoped_lock lock(this->mutex);
    while(this->read_lock > 0 || this->write_lock)
        // wait unlocks the mutex and reacquires the lock when it is notified
        this->cv.wait(lock);
    this->read_lock = 1;
    this->write_lock = true;
}

template<typename T>
void media_buffer_lockable<T>::lock_read()
{
    scoped_lock lock(this->mutex);
    while(this->write_lock)
        this->cv.wait(lock);
    this->read_lock++;
}

template<typename T>
void media_buffer_lockable<T>::unlock_write()
{
    scoped_lock lock(this->mutex);
    assert_(this->write_lock);
    assert_(this->read_lock > 0);
    this->write_lock = false;
    this->cv.notify_all();
}

template<typename T>
void media_buffer_lockable<T>::unlock()
{
    scoped_lock lock(this->mutex);
    this->write_lock = false;
    this->read_lock--;
    assert_(this->read_lock >= 0);
    this->cv.notify_all();
}

template<typename T>
bool media_buffer_lockable<T>::is_locked() const
{
    scoped_lock lock(this->mutex);
    return (this->write_lock || this->read_lock);
}

typedef media_buffer_lockable<media_buffer_texture> media_buffer_lockable_texture;
typedef std::shared_ptr<media_buffer_lockable_texture> media_buffer_lockable_texture_t;


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample::media_sample(time_unit timestamp) : timestamp(timestamp)
{
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


//media_sample_texture::media_sample_texture(const media_buffer_texture_t& texture_buffer) :
//    buffer(texture_buffer)
//{
//}


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