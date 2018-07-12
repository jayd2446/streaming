#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <deque>
#include <condition_variable>
#include <stdint.h>
#include <d3d11.h>
#include <atlbase.h>
#include <mfidl.h>
#include "assert.h"

#define SECOND_IN_TIME_UNIT 10000000

// 100 nanosecond = 1 time_unit
typedef int64_t time_unit;
// frame unit is used to accurately represent a frame position
// relative to the time source
typedef int64_t frame_unit;

class media_buffer
{
public:
    // unique lock is the same as scoped lock
    typedef std::unique_lock<std::mutex> scoped_lock;
private:
    std::atomic_int_fast32_t views;

    volatile int read_lock;
    volatile bool write_lock;
    std::condition_variable cv;
    std::mutex mutex;
public:
    media_buffer();
    virtual ~media_buffer();

    // waits for write and read to complete
    void lock();
    // waits only for the write to complete
    void lock_read();

    // unlocks the write lock
    void unlock_write();
    // unlocks the write and read lock
    void unlock();
};

typedef std::shared_ptr<media_buffer> media_buffer_t;

class media_sample
{
public:
    media_buffer_t buffer;
    time_unit timestamp;

    media_sample(time_unit timestamp = 0) : timestamp(timestamp) {}
    explicit media_sample(const media_buffer_t& buffer, time_unit timestamp = 0);
};

// h264 memory buffer
class media_buffer_h264 : public media_buffer
{
public:
    CComPtr<IMFSample> sample;
};

typedef std::shared_ptr<media_buffer_h264> media_buffer_h264_t;

// aac memory buffer
class media_buffer_aac : public media_buffer
{
public:
    CComPtr<IMFSample> sample;
};

typedef std::shared_ptr<media_buffer_aac> media_buffer_aac_t;

// TODO: parameters should be moved from the buffer class to sample class
// samples buffer
class media_buffer_samples : public media_buffer
{
public:
    // bit depth is assumed to be 16 bits always
    UINT32 channels, bit_depth, sample_rate;
    std::deque<CComPtr<IMFSample>> samples;
};

typedef std::shared_ptr<media_buffer_samples> media_buffer_samples_t;

class media_buffer_texture : public media_buffer
{
public:
    CComPtr<ID3D11Texture2D> texture;
    CComPtr<IDXGIResource> resource;
};

typedef std::shared_ptr<media_buffer_texture> media_buffer_texture_t;

// implements raii-based locking for the buffers in the sample
class media_sample_view
{
public:
    enum view_lock_t
    {
        READ_LOCK_BUFFERS,
        LOCK_BUFFERS
    };
private:
public:
    media_sample sample;

    // for gradual media sample view refactoring
    media_sample_view() : sample(NULL, 0) {}


    explicit media_sample_view(const media_buffer_t& buffer, view_lock_t = LOCK_BUFFERS);
    virtual ~media_sample_view();

    template<typename T>
    std::shared_ptr<T> get_buffer() const {return std::dynamic_pointer_cast<T>(this->sample.buffer);}
};

typedef std::shared_ptr<media_sample_view> media_sample_view_t;

// TODO: dynamic buffer casting should be changed to this
class media_sample_view_texture : public media_sample_view
{
private:
public:
    const media_buffer_texture_t texture_buffer;
    explicit media_sample_view_texture(
        const media_buffer_texture_t& texture_buffer, view_lock_t = LOCK_BUFFERS);
};

typedef std::shared_ptr<media_sample_view_texture> media_sample_view_texture_t;

template<typename T>
inline std::shared_ptr<T> cast(const media_sample_view_t& sample_view)
{
    return std::dynamic_pointer_cast<T>(sample_view);
}



// TODO: maybe change media samples to struct to indicate
// that they must be copy constructable
class media_sample_
{
public:
    time_unit timestamp;
    explicit media_sample_(time_unit timestamp = -1);
    virtual ~media_sample_() {}
};

// TODO: make a generalized sample class that only includes a buffer;
// that class will typedef a sample texture and sample h264 classes

class media_sample_texture_ : public media_sample_
{
public:
    typedef media_buffer_texture_t buffer_t;
public:
    media_buffer_texture_t buffer;
    media_sample_texture_() {}
    explicit media_sample_texture_(const media_buffer_texture_t& texture_buffer);
};

class media_sample_h264 : public media_sample_
{
public:
    typedef media_buffer_h264_t buffer_t;
public:
    media_buffer_h264_t buffer;
    media_sample_h264() {}
    explicit media_sample_h264(const media_buffer_h264_t& buffer);
};

class media_sample_view_base
{
protected:
    static void deleter(media_buffer*);
public:
    virtual ~media_sample_view_base() {}
};


template<typename Sample>
class media_sample_view_ : public media_sample_view_base
{
public:
    typedef Sample sample_t;
    typedef media_sample_view::view_lock_t view_lock_t;
private:
    // this could be replaced with a reference counter in media_buffer,
    // but this works fine aswell
    std::shared_ptr<media_buffer> buffer_view;

    void lock(view_lock_t);
public:
    // TODO: rename to sample and remove sample
    // from media sample view;
    
    // this also ensures that the buffer which is referenced by
    // buffer_view won't be deleted prematurely
    sample_t sample;

    media_sample_view_() {}
    explicit media_sample_view_(const sample_t& sample, view_lock_t = view_lock_t::LOCK_BUFFERS);
    ~media_sample_view_();

    void attach(const sample_t& sample, view_lock_t = LOCK_BUFFERS);
    void attach(const typename sample_t::buffer_t& buffer, view_lock_t = view_lock_t::LOCK_BUFFERS);
    void detach();
};

typedef media_sample_view_<media_sample_texture_> media_sample_view_texture_;
typedef media_sample_view_<media_sample_h264> media_sample_view_h264;

template<typename T>
media_sample_view_<T>::media_sample_view_(const sample_t& sample, view_lock_t view_lock) :
    sample(sample),
    buffer_view(sample.buffer.get(), &media_sample_view_base::deleter)
{
    this->lock(view_lock);
}

template<typename T>
media_sample_view_<T>::~media_sample_view_()
{
    this->detach();
}

template<typename T>
void media_sample_view_<T>::lock(view_lock_t view_lock)
{
    if(view_lock == view_lock_t::READ_LOCK_BUFFERS)
        this->buffer_view->lock_read();
    else if(view_lock == view_lock_t::LOCK_BUFFERS)
        this->buffer_view->lock();
    else
        assert_(false);
}

template<typename T>
void media_sample_view_<T>::attach(const sample_t& sample, view_lock_t view_lock)
{
    this->detach();

    this->sample = sample;
    this->buffer_view.reset(sample.buffer.get(), &media_sample_view_base::deleter);

    this->lock(view_lock);
}

template<typename T>
void media_sample_view_<T>::attach(const typename sample_t::buffer_t& buffer, view_lock_t view_lock)
{
    this->detach();

    this->sample.buffer = buffer;
    this->buffer_view.reset(buffer.get(), &media_sample_view_base::deleter);

    this->lock(view_lock);
}

template<typename T>
void media_sample_view_<T>::detach()
{
    this->buffer_view.reset((media_buffer*)NULL, &media_sample_view_::deleter);
    this->sample.buffer.reset();
}