#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include "presentation_clock.h"
#include "transform_aac_encoder.h"
#include "transform_audioprocessor.h"
#include "control_pipeline.h"
#include <Audioclient.h>
#include <mfapi.h>
#include <memory>
#include <deque>
#include <list>
#include <mutex>
#include <atomic>
#include <string>

#pragma comment(lib, "Mfplat.lib")

#define BUFFER_DURATION (SECOND_IN_TIME_UNIT / 2) // 500ms buffer
// wasapi is always 32 bit float in shared mode
#define WASAPI_BITS_PER_SAMPLE 32

class source_wasapi;
typedef std::shared_ptr<source_wasapi> source_wasapi_t;

struct IMMDevice;

// TODO: non live sources will feed data to audio processor in
// batches that cover the time between old and new request times
// (silence source will be an example of that)

class source_wasapi : public media_source
{
    friend class stream_wasapi;
public:
    typedef async_callback<source_wasapi> async_callback_t;
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef std::list<CComPtr<IMFSample>> buffer_t;

    // wasapi is always 32 bit float in shared mode
    typedef float bit_depth_t;
    // the interval of request pumping when the component has broken
    static const INT64 request_pump_interval_ms = 200;//40;

    struct empty {};
    typedef request_queue<empty> request_queue;
    typedef request_queue::request_t request_t;
private:
    control_pipeline_t ctrl_pipeline;

    CComPtr<IAudioClient> audio_client, audio_client_render;
    CComPtr<IAudioCaptureClient> audio_capture_client;
    CComPtr<IAudioRenderClient> audio_render_client;
    CHandle process_event;
    MFWORKITEM_KEY process_work_key;
    DWORD work_queue_id;

    std::mutex process_mutex, serve_mutex;
    CComPtr<async_callback_t> process_callback;
    CComPtr<async_callback_t> serve_callback;

    request_queue requests;

    frame_unit consumed_samples_end;
    REFERENCE_TIME buffer_actual_duration;
    UINT32 render_buffer_frame_count;

    frame_unit next_frame_position;
    frame_unit frame_base, devposition_base;

    bool started, capture, broken, wait_queue;

    std::mutex raw_buffer_mutex, buffer_mutex;
    buffer_t raw_buffer, buffer;

    CComPtr<IMFMediaType> waveformat_type;
    // maximum number of audio frames(aka pcm samples) the allocated buffer can hold
    UINT32 block_align, samples_per_second, channels;

    HRESULT add_event_to_wait_queue();
    HRESULT create_waveformat_type(WAVEFORMATEX*);
    // https://blogs.msdn.microsoft.com/matthew_van_eerde/2008/12/10/sample-playing-silence-via-wasapi-event-driven-pull-mode/
    // https://github.com/mvaneerde/blog/blob/master/silence/silence/silence.cpp
    HRESULT play_silence();
    void serve_requests();

    // handles cutting operations
    void serve_cb(void*);
    // callback for new audio data
    void process_cb(void*);

    //  plays silence in loopback devices so that sample positions
    // stay consistent in regard to time and the source loopback pushes
    // samples to downstream consistently
    HRESULT initialize_render(IMMDevice*, WAVEFORMATEX*);

    static void make_silence(UINT32 frames, UINT32 channels, bit_depth_t* buffer);
public:
    explicit source_wasapi(const media_session_t& session);
    ~source_wasapi();

    void initialize(
        const control_pipeline_t&, 
        const std::wstring& device_id, bool capture);
    media_stream_t create_stream();
};

class stream_wasapi : public media_stream
{
    friend class source_wasapi;
public:
    /*typedef source_wasapi::scoped_lock scoped_lock;*/
private:
    source_wasapi_t source;
    media_buffer_samples_t audio_buffer;
public:
    explicit stream_wasapi(const source_wasapi_t& source);

    bool get_clock(presentation_clock_t& c) {return this->source->session->get_current_clock(c);}

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_wasapi> stream_wasapi_t;