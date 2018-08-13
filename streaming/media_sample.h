#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <deque>
#include <list>
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

// TODO: locking should not be included in every type
class media_buffer
{
public:
    // unique lock is the same as scoped lock
    typedef std::unique_lock<std::mutex> scoped_lock;
private:
    volatile int read_lock;
    volatile bool write_lock;
    std::condition_variable cv;
    mutable std::mutex mutex;
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

    bool is_locked() const;
};

typedef std::shared_ptr<media_buffer> media_buffer_t;

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

class media_buffer_samples : public media_buffer
{
public:
    std::deque<CComPtr<IMFSample>> samples;
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
};

typedef std::shared_ptr<media_buffer_texture> media_buffer_texture_t;

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
    explicit media_sample_texture(const media_buffer_texture_t& texture_buffer);
};

class media_sample_h264 : public media_sample
{
public:
    typedef media_buffer_h264_t buffer_t;
public:
    media_buffer_h264_t buffer;
    media_sample_h264() {}
    explicit media_sample_h264(const media_buffer_h264_t& buffer);
};

class media_sample_audio : public media_sample
{
public:
    typedef media_buffer_samples_t buffer_t;
public:
    // the (sample pos + sample dur) max of buffer
    frame_unit frame_end;
    // bit depth is assumed to be 16 bits always
    UINT32 channels, bit_depth, sample_rate;
    media_buffer_samples_t buffer;

    media_sample_audio() {}
    explicit media_sample_audio(const media_buffer_samples_t& buffer);

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
};

enum class view_lock_t
{
    READ_LOCK_BUFFERS,
    LOCK_BUFFERS
};

// TODO: media_sample_view_ objects should be dynamic castable to
// their underlying types(inherit from Sample);
// also, rename sample_view from request_queue to sample
template<typename Sample>
class media_sample_view : public media_sample
{
private:
    static void deleter(media_buffer*);
public:
    typedef Sample sample_t;
private:
    // this could be replaced with a reference counter in media_buffer,
    // but this works fine aswell
    std::shared_ptr<media_buffer> buffer_view;

    void lock(view_lock_t);
public:
    // this also ensures that the buffer which is referenced by
    // buffer_view won't be deleted prematurely
    sample_t sample;

    media_sample_view() {}
    explicit media_sample_view(const sample_t& sample, view_lock_t = view_lock_t::LOCK_BUFFERS);
    ~media_sample_view();

    void attach(const sample_t& sample, view_lock_t = view_lock_t::LOCK_BUFFERS);
    void attach(const typename sample_t::buffer_t& buffer, view_lock_t = view_lock_t::LOCK_BUFFERS);
    void detach();
};

typedef media_sample_view<media_sample_texture> media_sample_view_texture;
typedef media_sample_view<media_sample_h264> media_sample_view_h264;

template<typename T>
media_sample_view<T>::media_sample_view(const sample_t& sample, view_lock_t view_lock) :
    sample(sample),
    buffer_view(sample.buffer.get(), deleter)
{
    this->lock(view_lock);
}

template<typename T>
media_sample_view<T>::~media_sample_view()
{
    this->detach();
}

template<typename T>
void media_sample_view<T>::lock(view_lock_t view_lock)
{
    if(view_lock == view_lock_t::READ_LOCK_BUFFERS)
        this->buffer_view->lock_read();
    else if(view_lock == view_lock_t::LOCK_BUFFERS)
        this->buffer_view->lock();
    else
        assert_(false);
}

template<typename T>
void media_sample_view<T>::attach(const sample_t& sample, view_lock_t view_lock)
{
    this->detach();

    this->sample = sample;
    this->buffer_view.reset(sample.buffer.get(), deleter);

    this->lock(view_lock);
}

template<typename T>
void media_sample_view<T>::attach(const typename sample_t::buffer_t& buffer, view_lock_t view_lock)
{
    this->detach();

    this->sample.buffer = buffer;
    this->buffer_view.reset(buffer.get(), deleter);

    this->lock(view_lock);
}

template<typename T>
void media_sample_view<T>::detach()
{
    this->buffer_view.reset((media_buffer*)NULL, deleter);
    this->sample.buffer.reset();
}

template<typename T>
void media_sample_view<T>::deleter(media_buffer* buffer)
{
    if(buffer)
        buffer->unlock();
}