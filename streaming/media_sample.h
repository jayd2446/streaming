#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <stdint.h>
#include <d3d11.h>
#include <atlbase.h>

// 100 nanosecond = 1 time_unit
typedef int64_t time_unit;

// TODO: media sample must be refactored for a better access to the texture

class media_sample
{
private:
     volatile bool available;
     std::condition_variable cv;
     std::mutex mutex;
public:
    media_sample();

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