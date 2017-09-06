#pragma once
#include <memory>
#include <vector>
#include <stdint.h>

class media_sample
{
private:
public:
    // in 100 nanosecond units
    int64_t timestamp;
};

typedef std::shared_ptr<media_sample> media_sample_t;
typedef std::vector<media_sample_t> media_samples;
typedef std::shared_ptr<media_samples> media_samples_t;