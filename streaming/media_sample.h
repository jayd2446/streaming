#pragma once
#include <memory>
#include <vector>
#include <stdint.h>
#include <d3d11.h>
#include <atlbase.h>

// 100 nanosecond = 1 time_unit
typedef int64_t time_unit;

class media_sample
{
private:
public:
    time_unit timestamp;
    CComPtr<ID3D11Texture2D> frame;
};

typedef std::shared_ptr<media_sample> media_sample_t;
typedef std::vector<media_sample_t> media_samples;
typedef std::shared_ptr<media_samples> media_samples_t;