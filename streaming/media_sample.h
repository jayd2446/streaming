#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <deque>
#include <stack>
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

template<typename Buffer>
class media_buffer_trackable : public Buffer, public enable_shared_from_this
{
public:
    typedef Buffer buffer_raw_t;
    typedef std::shared_ptr<Buffer> buffer_t;
    // unique lock is the same as scoped lock
    typedef std::unique_lock<std::mutex> scoped_lock;
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

template<typename Buffer>
class media_buffer_pooled : public media_buffer_trackable<Buffer>
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef std::shared_ptr<std::stack<std::shared_ptr<media_buffer_pooled>>> samples_pool;
private:
    std::shared_ptr<std::recursive_mutex> available_samples_mutex;
    samples_pool available_samples;

    void on_delete();
public:
    media_buffer_pooled(const samples_pool&, const std::shared_ptr<std::recursive_mutex>&);
    buffer_t create_pooled_buffer();
};

template<typename T>
media_buffer_pooled<T>::media_buffer_pooled(
    const samples_pool& available_samples,
    const std::shared_ptr<std::recursive_mutex>& available_samples_mutex) :
    available_samples(available_samples), available_samples_mutex(available_samples_mutex)
{
}

template<typename T>
void media_buffer_pooled<T>::on_delete()
{
    // TODO: pushing to a container might introduce a cyclic dependency situation

    // move the buffer back to sample pool
    scoped_lock lock(*this->available_samples_mutex);
    this->available_samples->push(this->shared_from_this<media_buffer_pooled>());
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
    DWORD texture_buffer_length;
    std::unique_ptr<BYTE[]> texture_buffer;

    CComPtr<ID3D11Texture2D> texture;
    virtual ~media_buffer_texture() {}
};

typedef std::shared_ptr<media_buffer_texture> media_buffer_texture_t;

typedef media_buffer_pooled<media_buffer_texture> media_buffer_pooled_texture;
typedef std::shared_ptr<media_buffer_pooled_texture> media_buffer_pooled_texture_t;

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