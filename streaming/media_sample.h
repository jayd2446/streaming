#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <deque>
#include <list>
#include <condition_variable>
#include <functional>
#include <stdint.h>
#include <d3d11.h>
#include <atlbase.h>
#include <mfidl.h>
#include "assert.h"
#include "enable_shared_from_this.h"

#undef max
#undef min

#define SECOND_IN_TIME_UNIT 10000000

// 100 nanosecond = 1 time_unit
typedef int64_t time_unit;
// frame unit is used to accurately represent a frame position
// relative to the time source
typedef int64_t frame_unit;

class media_buffer
{
public:
    virtual ~media_buffer() {}
};

typedef std::shared_ptr<media_buffer> media_buffer_t;

enum class buffer_lock_t
{
    READ_LOCK,
    LOCK
};

// TODO: lockable buffer should probably encapsulate the real buffer
template<typename Buffer>
class media_buffer_lockable : public Buffer, public enable_shared_from_this
{
public:
    typedef Buffer buffer_raw_t;
    typedef std::shared_ptr<Buffer> buffer_t;
    // unique lock is the same as scoped lock
    typedef std::unique_lock<std::mutex> scoped_lock;
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

    // the deleter unlocks the Buffer
    void deleter(buffer_raw_t*);
public:
    media_buffer_lockable();
    virtual ~media_buffer_lockable();

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
void media_buffer_lockable<T>::deleter(buffer_raw_t* buffer)
{
    assert_(buffer == this);
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

    // the lock buffer will stay alive at least as long as the wrapped buffer is alive
    using std::placeholders::_1;
    auto deleter_f = std::bind(&media_buffer_lockable::deleter, 
        this->shared_from_this<media_buffer_lockable>(), _1);
    return buffer_t(this, deleter_f);
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




// h264 memory buffer
class media_buffer_h264 : public media_buffer
{
public:
    CComPtr<IMFSample> sample;
    virtual ~media_buffer_h264() {}
};

typedef std::shared_ptr<media_buffer_h264> media_buffer_h264_t;

// aac memory buffer
class media_buffer_aac : public media_buffer
{
public:
    CComPtr<IMFSample> sample;
    virtual ~media_buffer_aac() {}
};

typedef std::shared_ptr<media_buffer_aac> media_buffer_aac_t;

class media_buffer_samples : public media_buffer
{
public:
    std::deque<CComPtr<IMFSample>> samples;
    virtual ~media_buffer_samples() {}
};

typedef std::shared_ptr<media_buffer_samples> media_buffer_samples_t;

class media_buffer_texture : public media_buffer
{
public:
    // intermediate texture stores the real texture
    // and texture is just a shared handle when using
    // multiple devices
    CComPtr<ID3D11Texture2D> intermediate_texture;
    CComPtr<ID3D11Texture2D> texture;
    virtual ~media_buffer_texture() {}
};

typedef std::shared_ptr<media_buffer_texture> media_buffer_texture_t;

typedef media_buffer_lockable<media_buffer_texture> media_buffer_lockable_texture;
typedef std::shared_ptr<media_buffer_lockable_texture> media_buffer_lockable_texture_t;

// TODO: maybe change media samples to struct to indicate
// that they must be copy constructable
class media_sample
{
public:
    time_unit timestamp;
    explicit media_sample(time_unit timestamp = -1);
    virtual ~media_sample() {}
};

class media_sample_texture : public media_sample
{
public:
    typedef media_buffer_texture_t buffer_t;
public:
    media_buffer_texture_t buffer;
    media_sample_texture() {}
    explicit media_sample_texture(const media_buffer_texture_t& buffer);
    virtual ~media_sample_texture() {}
};

class media_sample_h264 : public media_sample
{
public:
    typedef media_buffer_h264_t buffer_t;
public:
    media_buffer_h264_t buffer;
    media_sample_h264() {}
    explicit media_sample_h264(const media_buffer_h264_t& buffer);
    virtual ~media_sample_h264() {}
};

class media_sample_audio : public media_sample
{
public:
    typedef media_buffer_samples_t buffer_t;
public:
    // the (sample pos + sample dur) max of buffer;
    // the frame_end is considered invalid if the buffer is empty,
    // unless the silent flag is set
    frame_unit frame_end;
    bool silent;
    // bit depth is assumed to be 16 bits always
    UINT32 channels, bit_depth, sample_rate;
    media_buffer_samples_t buffer;

    media_sample_audio() {}
    explicit media_sample_audio(const media_buffer_samples_t& buffer);
    virtual ~media_sample_audio() {}

    UINT32 get_block_align() const {return this->bit_depth / 8 * this->channels;}
};

class media_sample_aac : public media_sample
{
public:
    typedef media_buffer_samples_t buffer_t;
public:
    // bit depth is assumed to be 16 bits always
    UINT32 channels, bit_depth, sample_rate;
    media_buffer_samples_t buffer;

    media_sample_aac() {}
    explicit media_sample_aac(const media_buffer_samples_t& buffer);
    virtual ~media_sample_aac() {}
};