#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "request_packet.h"
#include <mfapi.h>
#include <memory>
#include <mutex>
#include <deque>

#pragma comment(lib, "Mfplat.lib")

#define OUT_BUFFER_FRAMES 1024

// resamples and cuts the audio

class transform_audioprocessor : public media_source
{
    friend class stream_audioprocessor;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef request_queue::request_t request_t;
private:
    bool running;
    CComPtr<IMFTransform> processor;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    CComPtr<IMFMediaType> input_type, output_type;
    UINT32 channels, sample_rate, block_align;
    std::recursive_mutex set_type_mutex;

    request_queue requests, requests_resample;
    std::recursive_mutex samples_mutex, process_mutex;
    std::deque<CComPtr<IMFSample>> samples;

    frame_unit sample_base;
    frame_unit next_sample_pos;

    void reset_input_type(UINT channels, UINT sample_rate, UINT bit_depth);
    bool resampler_process_output(IMFSample*);
    // resamples all the samples and pushes them to samples container
    void resample(const media_buffer_samples_t&, const request_packet&);
    // tries to serve the request queue
    void try_serve();
public:
    explicit transform_audioprocessor(const media_session_t& session);

    void initialize();
    media_stream_t create_stream();
};

typedef std::shared_ptr<transform_audioprocessor> transform_audioprocessor_t;

class stream_audioprocessor : public media_stream
{
public:
    typedef transform_audioprocessor::scoped_lock scoped_lock;
    typedef async_callback<stream_audioprocessor> async_callback_t;
private:
    transform_audioprocessor_t transform;
    CComPtr<async_callback_t> process_callback;

    void processing_cb(void*);
public:
    explicit stream_audioprocessor(const transform_audioprocessor_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};