#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <deque>
#include <stack>
#include <utility>
#include <condition_variable>
#include <functional>
#include <stdint.h>
#include <d3d11.h>
#include <d2d1_1.h>
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

frame_unit convert_to_frame_unit(time_unit, frame_unit frame_rate_num, frame_unit frame_rate_den);
time_unit convert_to_time_unit(frame_unit, frame_unit frame_rate_num, frame_unit frame_rate_den);

class media_buffer : public enable_shared_from_this
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

// buffer must inherit from enable_shared_from_this
template<typename Buffer>
class media_buffer_trackable : public Buffer
{
public:
    typedef Buffer buffer_raw_t;
    typedef std::shared_ptr<Buffer> buffer_t;
private:
    void deleter(buffer_raw_t*);
protected:
    virtual void on_delete() = 0;
    buffer_t create_tracked_buffer();
public:
    virtual ~media_buffer_trackable() {}
};

template<typename T>
void media_buffer_trackable<T>::deleter(buffer_raw_t* buffer)
{
    assert_(buffer == this);
    buffer;
    this->on_delete();
}

template<typename T>
typename media_buffer_trackable<T>::buffer_t
media_buffer_trackable<T>::create_tracked_buffer()
{
    // media_buffer_trackable will stay alive at least as long as the wrapped buffer is alive
    using std::placeholders::_1;
    auto deleter_f = std::bind(&media_buffer_trackable::deleter,
        this->shared_from_this<media_buffer_trackable>(), _1);

    return buffer_t(this, deleter_f);
}

template<typename PooledBuffer>
class buffer_pool
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef PooledBuffer pooled_buffer_t;
    typedef std::stack<std::shared_ptr<pooled_buffer_t>> buffer_pool_t;
private:
    volatile bool disposed;
public:
    buffer_pool();

    // mutex must be locked when using buffer_pool methods
    std::recursive_mutex mutex;
    buffer_pool_t container;

    // the pool must be manually disposed;
    // it breaks the circular dependency between the pool and its objects;
    // failure to dispose causes memory leak
    void dispose();
    bool is_disposed() const {return this->disposed;}
};

template<typename T>
buffer_pool<T>::buffer_pool() : disposed(false)
{
}

template<typename T>
void buffer_pool<T>::dispose()
{
    assert_(!this->disposed);
    this->disposed = true;
    buffer_pool_t().swap(this->container);
}

template<typename Buffer>
class media_buffer_pooled : public media_buffer_trackable<Buffer>
{
public:
    typedef buffer_pool<media_buffer_pooled> buffer_pool;
private:
    std::shared_ptr<buffer_pool> pool;
    void on_delete();
public:
    explicit media_buffer_pooled(const std::shared_ptr<buffer_pool>& pool);
    buffer_t create_pooled_buffer();
};

template<typename T>
media_buffer_pooled<T>::media_buffer_pooled(const std::shared_ptr<buffer_pool>& pool) :
    pool(pool)
{
}

template<typename T>
void media_buffer_pooled<T>::on_delete()
{
    // move the buffer back to sample pool if the pool isn't disposed yet;
    // otherwise, this object will be destroyed after the std bind releases the last reference
    buffer_pool::scoped_lock lock(this->pool->mutex);
    if(!this->pool->is_disposed())
        this->pool->container.push(this->shared_from_this<media_buffer_pooled>());
}

template<typename T>
typename media_buffer_pooled<T>::buffer_t media_buffer_pooled<T>::create_pooled_buffer()
{
    return this->create_tracked_buffer();
}

// h264 memory buffer
// TODO: just use media_buffer_samples and remove h264&aac buffers
class media_buffer_h264 : public media_buffer
{
public:
    std::deque<CComPtr<IMFSample>> samples;
    /*CComPtr<IMFSample> sample;*/
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
    // TODO: the memory buffer should be a derived class of this defined in h264 encoder;
    // rename the texture_buffer to memory_buffer
    DWORD texture_buffer_length;
    std::unique_ptr<BYTE[]> texture_buffer;

    CComPtr<ID2D1Bitmap1> bitmap;
    CComPtr<ID3D11Texture2D> texture;
    virtual ~media_buffer_texture() {}
};

typedef std::shared_ptr<media_buffer_texture> media_buffer_texture_t;

typedef media_buffer_pooled<media_buffer_texture> media_buffer_pooled_texture;
typedef std::shared_ptr<media_buffer_pooled_texture> media_buffer_pooled_texture_t;

class media_buffer_textures : public media_buffer
{
public:
    // every element must be valid
    std::deque<std::pair<frame_unit /*frame pos*/, media_buffer_texture_t>> frames;
    virtual ~media_buffer_textures() {}
};

typedef std::shared_ptr<media_buffer_textures> media_buffer_textures_t;

// TODO: maybe change media samples to struct to indicate
// that they must be copy constructable
class media_sample
{
public:
    time_unit timestamp;
    explicit media_sample(time_unit timestamp = -1);
    virtual ~media_sample() {}
};

//class media_sample_texture : public media_sample
//{
//public:
//    typedef media_buffer_texture_t buffer_t;
//public:
//    media_buffer_texture_t buffer;
//    media_sample_texture() {}
//    explicit media_sample_texture(const media_buffer_texture_t& buffer);
//    virtual ~media_sample_texture() {}
//};

//class media_sample_textures : public media_sample
//{
//public:
//    typedef media_buffer_textures_t buffer_t;
//public:
//    media_buffer_textures_t buffer;
//};

class media_sample_h264 : public media_sample
{
public:
    typedef media_buffer_h264_t buffer_t;
public:
    // for debugging
    bool software;
    media_buffer_h264_t buffer;
    media_sample_h264() {}
    explicit media_sample_h264(const media_buffer_h264_t& buffer);
    virtual ~media_sample_h264() {}
};

// TODO: the buffer contained in sample should be read only
// base class for uncompressed audio and video
// TODO: audio could also use a buffer pool for audio samples
class media_sample_frames : public media_sample
{
public:
    // TODO: this could include a framerate

    // fields are invalid if is_null
    frame_unit frame_start, frame_end;
    // silent flag overrides non empty buffers
    bool silent;

    media_sample_frames() : silent(false) {}
    virtual ~media_sample_frames() {}
    virtual bool is_null() const = 0;
};

class media_sample_video : public media_sample_frames
{
public:
    typedef media_buffer_textures_t buffer_t;
public:
    media_buffer_textures_t buffer;
    // single buffer can be used instead of multiple buffers
    media_buffer_texture_t single_buffer;

    media_sample_video() {}
    explicit media_sample_video(const media_buffer_texture_t& single_buffer) : 
        single_buffer(single_buffer) {}
    virtual ~media_sample_video() {}

    // null sample doesn't have any data;
    // it is used in conjunction with multiple frames
    bool is_null() const {return (!this->buffer || this->buffer->frames.empty()) &&
        (!this->single_buffer || !this->single_buffer->texture) && !this->silent;}
};

// TODO: videoprocessor sample class inherits from media sample video

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