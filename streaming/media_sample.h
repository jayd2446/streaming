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
    virtual ~media_buffer() {}

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
};

typedef std::shared_ptr<media_sample> media_sample_t;

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
    // TODO: when obsolete code is cleaned up enum class is not needed anymore
    enum view_lock_t
    {
        READ_LOCK_BUFFERS,
        LOCK_BUFFERS
    };
private:
    media_sample_t sample;
    view_lock_t view_lock;
public:
    explicit media_sample_view(const media_sample_t& sample, view_lock_t = LOCK_BUFFERS);
    ~media_sample_view();

    const media_sample_t& get_sample() const {return this->sample;}
    template<typename T>
    std::shared_ptr<T> get_buffer() const {return std::dynamic_pointer_cast<T>(this->sample->buffer);}
};

typedef std::shared_ptr<const media_sample_view> media_sample_view_t;