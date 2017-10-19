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

    volatile bool read_lock, write_lock;
public:
    media_sample();
    virtual ~media_sample() {}

    time_unit timestamp;

    // waits for write and read to complete
    void lock_sample_();
    // waits only for the write to complete
    void read_lock_sample();

    // unlocks the write lock
    void unlock_write_lock_sample();
    // unlocks the write and read lock
    void unlock_sample();

    // TODO: these are obsolete

    // tries to lock the sample without blocking
    bool try_lock_sample();
    // waits for the sample to become available and sets it to unavailable
    void lock_sample();
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
    HANDLE shared_handle;
};

typedef std::shared_ptr<media_sample_texture> media_sample_texture_t;

// implements raii-based locking for the sample
class media_sample_view
{
public:
    // TODO: when obsolete code is cleaned up enum class is not needed anymore
    enum class view_lock_t
    {
        READ_LOCK_SAMPLE,
        LOCK_SAMPLE
    };
private:
    media_sample_t sample;
    view_lock_t view_lock;
public:
    media_sample_view(const media_sample_t& sample, view_lock_t);
    // TODO: obsolete
    media_sample_view(const media_sample_t& sample, bool already_locked = false);
    ~media_sample_view();

    // TODO: sample view can have an enum that identifies the
    // sample subtype

    const media_sample_t& get_sample() const {return this->sample;}
    template<typename T>
    std::shared_ptr<T> get_sample() const {return std::dynamic_pointer_cast<T>(this->sample);}
};

typedef std::shared_ptr<const media_sample_view> media_sample_view_t;