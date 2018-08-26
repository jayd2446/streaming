#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "request_packet.h"
#include <mfapi.h>
#include <memory>
#include <mutex>
#include <deque>

#pragma comment(lib, "Mfplat.lib")

// resamples the audio

// TODO: if input type is reset, the samples buffer must be dismissed
class transform_audioprocessor : public media_source
{
    friend class stream_audioprocessor;
public:
    struct packet
    {
        media_sample_audio audio;
        bool drain;
    };
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<transform_audioprocessor> async_callback_t;
    typedef request_queue<packet> request_queue;
    typedef request_queue::request_t request_t;

    // audio processor outputs 16 bit audio because
    // the mixer mixes in 16 bit mode only
    typedef int16_t bit_depth_t;
    /*typedef float bit_depth_t;*/
private:
    bool running;
    CComPtr<IMFTransform> processor;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    CComPtr<IMFMediaType> input_type, output_type;
    UINT32 channels, sample_rate, block_align;

    std::recursive_mutex resample_mutex;
    std::mutex process_mutex;

    frame_unit sample_base;
    frame_unit next_sample_pos, next_sample_pos_in;

    request_queue requests;

    void reset_input_type(UINT channels, UINT sample_rate, UINT bit_depth);
    bool resampler_process_output(IMFSample* sample);
    // resamples all the samples and pushes them to samples container
    void resample(media_sample_audio&, const media_sample_audio&, bool drain);

    void process();
public:
    explicit transform_audioprocessor(const media_session_t& session);

    void initialize();
    media_stream_t create_stream(presentation_clock_t&);
};

typedef std::shared_ptr<transform_audioprocessor> transform_audioprocessor_t;

class stream_audioprocessor : public media_stream_clock_sink
{
    friend class transform_audioprocessor;
public:
    typedef transform_audioprocessor::scoped_lock scoped_lock;
    typedef async_callback<stream_audioprocessor> async_callback_t;
private:
    transform_audioprocessor_t transform;
    media_buffer_samples_t audio_buffer;
    CComPtr<IMFSample> sample;
    CComPtr<IMFMediaBuffer> buffer;

    time_unit drain_point;

    void on_component_start(time_unit);
    void on_component_stop(time_unit);
public:
    explicit stream_audioprocessor(const transform_audioprocessor_t& transform);

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};