#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <stdint.h>
#include <d3d11.h>
#include <atlbase.h>
#include <mfidl.h>

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
    const media_buffer_t buffer;
    time_unit timestamp;

    explicit media_sample(const media_buffer_t& buffer, time_unit timestamp = 0);
};

// h264 memory buffer
class media_buffer_memorybuffer : public media_buffer
{
public:
    CComPtr<IMFSample> sample;
};

typedef std::shared_ptr<media_buffer_memorybuffer> media_buffer_memorybuffer_t;

// aac memory buffer
class media_buffer_aac : public media_buffer
{
public:
    CComPtr<IMFSample> sample;
};

typedef std::shared_ptr<media_buffer_aac> media_buffer_aac_t;

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





class media_sample_
{
public:
    time_unit timestamp;
};

class media_sample_texture_ : public media_sample_
{
public:
    media_sample_texture_()
    {
        this->buffer.reset(new media_buffer_texture);
    }

    media_buffer_texture_t buffer;
};

class media_sample_view_
{
private:
    static void deleter(media_buffer* buffer);
public:
    enum view_lock_t
    {
        READ_LOCK_BUFFERS,
        LOCK_BUFFERS
    };
private:
    // when the buffer view reference count reaches zero,
    // the custom deleter will unlock its lock
    std::shared_ptr<media_buffer> buffer_view;
    // this will ensure that the lifetime of the sample's buffer outlives
    // the view's
    media_buffer_t buffer;
public:
    // TODO: the sample is either dynamic casted or reinterpret casted
    media_sample_ sample;

    // TODO: a constructor that copies the sample when its an lvalue

    template<typename Sample>
    explicit media_sample_view_(Sample&& rvalue_sample, view_lock_t = LOCK_BUFFERS);
    ~media_sample_view_();
};



template<typename Sample>
media_sample_view_::media_sample_view_(Sample&& rvalue_sample, view_lock_t view_lock) :
    buffer(rvalue_sample.buffer),
    sample(std::move(rvalue_sample)),
    buffer_view(rvalue_sample.buffer.get(), &media_sample_view_::deleter)
{
    if(view_lock == READ_LOCK_BUFFERS)
        this->buffer_view->lock_read();
    else if(view_lock == LOCK_BUFFERS)
        this->buffer_view->lock();
}