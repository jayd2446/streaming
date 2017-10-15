#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <stdint.h>
#include <d3d11.h>
#include <atlbase.h>
#include <mfidl.h>

// 100 nanosecond = 1 time_unit
typedef int64_t time_unit;

class media_sample
{
public:
    // unique lock is the same as scoped lock
    typedef std::unique_lock<std::mutex> scoped_lock;
private:
    volatile bool available;
    std::condition_variable cv;
    std::mutex mutex;
public:
    media_sample();
    virtual ~media_sample() {}

    time_unit timestamp;

    // tries to lock the sample without blocking
    bool try_lock_sample();
    // waits for the sample to become available and sets it to unavailable
    void lock_sample();
    // makes the sample available
    void unlock_sample();
};

typedef std::shared_ptr<media_sample> media_sample_t;

// h264 memory buffer
class media_sample_memorybuffer : public media_sample
{
public:
    CComPtr<IMFSample> sample;
};

typedef std::shared_ptr<media_sample_memorybuffer> media_sample_memorybuffer_t;

class media_sample_texture : public media_sample
{
public:
    media_sample_texture();

    CComPtr<ID3D11Texture2D> texture;
    CComPtr<IDXGIResource> resource;
    CComPtr<IDXGIKeyedMutex> mutex;
    HANDLE shared_handle;
};

typedef std::shared_ptr<media_sample_texture> media_sample_texture_t;

// implements raii-based locking for the sample
class media_sample_view
{
private:
    media_sample_t sample;
public:
    media_sample_view(const media_sample_t& sample, bool already_locked = false);
    ~media_sample_view();

    // TODO: sample view can have an enum that identifies the
    // sample subtype

    const media_sample_t& get_sample() const {return this->sample;}
    template<typename T>
    std::shared_ptr<T> get_sample() const {return std::dynamic_pointer_cast<T>(this->sample);}
};

typedef std::shared_ptr<const media_sample_view> media_sample_view_t;