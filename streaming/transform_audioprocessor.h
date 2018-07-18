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

class source_loopback;

// resamples and cuts the audio

// TODO: if input type is reset, the samples buffer must be dismissed
class transform_audioprocessor : public media_source
{
    friend class stream_audioprocessor;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    struct empty {};
    typedef request_queue<empty> request_queue;
    typedef request_queue::request_t request_t;
    /*typedef std::deque<CComPtr<IMFSample>> sample_container;*/
    typedef async_callback<transform_audioprocessor> async_callback_t;
private:
    bool running;
    CComPtr<IMFTransform> processor;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    CComPtr<IMFMediaType> input_type, output_type;
    UINT32 channels, sample_rate, block_align;
    std::recursive_mutex set_type_mutex;

    source_loopback* audio_device;

    request_queue requests;
    std::recursive_mutex buffer_mutex, serve_mutex, resample_mutex;
    media_buffer_samples buffer;
    /*sample_container samples;*/
    media_buffer_samples_t unprocessed_samples;

    frame_unit sample_base;
    frame_unit next_sample_pos;
    frame_unit consumed_samples_end;

    void reset_input_type(UINT channels, UINT sample_rate, UINT bit_depth);
    bool resampler_process_output(IMFSample*);
    // resamples all the samples and pushes them to samples container
    void resample(const media_sample_audio&);
    // tries to serve the request queue
    void try_serve();
    // called by the audio device
    void serve_cb(void*);
public:
    CComPtr<async_callback_t> serve_callback;

    explicit transform_audioprocessor(const media_session_t& session);

    void initialize(source_loopback* audio_device);
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
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};