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

// TODO: media sample must be refactored for a better access to the texture
// (this can be accomplished by making a subclass that implements video memory access)

// TODO: media sample view which is used to lock and unlock the sample

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

    // tries to lock the sample without blocking
    bool try_lock_sample();
    // waits for the sample to become available and sets it to unavailable
    void lock_sample();
    // makes the sample available
    void unlock_sample();

    time_unit timestamp;
    HANDLE frame;
};

typedef std::shared_ptr<media_sample> media_sample_t;

class media_sample_memorybuffer : public media_sample
{
public:
    CComPtr<IMFMediaBuffer> buffer;
};

typedef std::shared_ptr<media_sample_memorybuffer> media_sample_memorybuffer_t;

// implements raii-based reference counted locking for the sample
class media_sample_view
{
private:
    media_sample_t sample;
public:
    media_sample_view(const media_sample_t& sample);
    ~media_sample_view();

    // TODO: sample view can have an enum that identifies the
    // sample subtype

    const media_sample_t& get_sample() const {return this->sample;}
    template<typename T>
    std::shared_ptr<T> get_sample() const {return std::dynamic_pointer_cast<T>(this->sample);}
};

typedef std::shared_ptr<media_sample_view> media_sample_view_t;