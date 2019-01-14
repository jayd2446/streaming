#pragma once

#include "media_sample.h"
#include <mfapi.h>
#include <vector>

#pragma comment(lib, "Mfplat.lib")

// resamples, maps channels and changes bit depth;
// audio resampler is very similar to aac encoder component

// not multithread safe
class audio_resampler
{
public:
    typedef buffer_pool<media_buffer_memory_pooled> buffer_pool_memory_t;
    // the duration of the resampled buffer that is preallocated
    // TODO: a buffer worth of second might be way too much
    static const time_unit buffer_duration = SECOND_IN_TIME_UNIT;// / 10;
private:
    bool initialized;
    UINT32 out_sample_rate, out_channels, out_bit_depth;
    UINT32 in_sample_rate, in_channels, in_bit_depth;

    CComPtr<IMFTransform> resampler;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    MFT_INPUT_STREAM_INFO input_stream_info;
    CComPtr<IMFMediaType> input_type, output_type;

    // keeps the hosts alive until the resampler has processed the input samples
    // TODO: the vector shouldn't allocate dynamic memory that much
    std::vector<media_buffer_memory_t> memory_hosts;
    std::shared_ptr<buffer_pool_memory_t> buffer_pool_memory;
    bool process_output(IMFSample* sample);
public:
    audio_resampler();
    ~audio_resampler();

    // TODO: resample only if resampling is actually needed
    void initialize(
        UINT32 out_sample_rate, UINT32 out_channels, UINT32 out_bit_depth,
        UINT32 in_sample_rate, UINT32 in_channels, UINT32 in_bit_depth);
    // drain should be used if there's a discontinuity in the original stream;
    // input parameter can be null;
    // returns the amount of frames added to container
    frame_unit resample(
        frame_unit frame_start_pos,
        const media_sample_audio_consecutive_frames&, 
        media_sample_audio_frames& container,
        bool drain);
};