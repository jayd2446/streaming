#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include "presentation_clock.h"
#include "transform_aac_encoder.h"
#include "transform_audiomixer2.h"
#include "audio_resampler.h"
#include <Audioclient.h>
#include <mfapi.h>
#include <memory>
#include <deque>
#include <list>
#include <mutex>
#include <atomic>
#include <string>
#include <queue>

#pragma comment(lib, "Mfplat.lib")

#define CAPTURE_BUFFER_DURATION (SECOND_IN_TIME_UNIT) // 1s buffer

class source_wasapi2;
typedef std::shared_ptr<source_wasapi2> source_wasapi2_t;

struct IMMDevice;

class source_wasapi2 : public media_source
{
    friend class stream_wasapi2;
public:
    typedef async_callback<source_wasapi2> async_callback_t;
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef buffer_pool<media_buffer_memory_pooled> buffer_pool_memory_t;
    typedef buffer_pool<media_sample_audio_frames_pooled> buffer_pool_audio_frames_t;

    // wasapi is always 32 bit float in shared mode
    typedef float bit_depth_t;
    static const INT64 capture_interval_ms = 40;
    // the timeout for the request after which it is considered stale
    // and dispatched without accompanying data
    static const time_unit request_timeout = SECOND_IN_TIME_UNIT;

    struct request_t
    {
        bool drain;
    };
    typedef request_queue<request_t> request_queue;
    /*typedef request_queue::request_t request_t;*/
private:
    control_class_t ctrl_pipeline;
    audio_resampler resampler;

    CComPtr<IAudioClient> audio_client, audio_client_render;
    CComPtr<IAudioCaptureClient> audio_capture_client;
    CComPtr<IAudioRenderClient> audio_render_client;

    CComPtr<async_callback_t> capture_callback;
    MFWORKITEM_KEY capture_work_key;
    DWORD work_queue_id;

    bool started, capture, in_wait_queue;

    frame_unit native_frame_base/*, devposition_base*/;
    frame_unit next_frame_position;
    bool set_new_frame_base;

    // request queue is allocated here so that newer requests won't move samples that
    // should be handled by older requests
    // TODO: recursive mutex probably unnecessary
    std::mutex requests_mutex;
    std::queue<request_queue::request_t> requests;

    std::mutex captured_audio_mutex;
    std::shared_ptr<buffer_pool_memory_t> buffer_pool_memory;
    std::shared_ptr<buffer_pool_audio_frames_t> buffer_pool_audio_frames;
    media_sample_audio_frames_t captured_audio;

    CHandle serve_callback_event;
    CComPtr<async_callback_t> serve_callback;
    CComPtr<IMFAsyncResult> serve_callback_result;
    MFWORKITEM_KEY serve_callback_key;
    bool serve_in_wait_queue;
    /*std::mutex serve_callback_mutex;*/

    CComPtr<IMFMediaType> waveformat_type;
    UINT32 block_align, samples_per_second, channels;
    REFERENCE_TIME buffer_actual_duration;
    UINT32 render_buffer_frame_count;
    UINT32 resampled_block_align;

    void sine_wave(BYTE* data, DWORD len);
    double sine_wave_counter;

    HRESULT queue_new_capture();
    void serve_cb(void*);
    void capture_cb(void*);
    HRESULT play_silence();
    //  plays silence in loopback devices so that sample positions
    // stay consistent in regard to time
    HRESULT initialize_render(IMMDevice*, WAVEFORMATEX*);
    HRESULT create_waveformat_type(WAVEFORMATEX*);
public:
    explicit source_wasapi2(const media_session_t& session);
    ~source_wasapi2();

    void initialize(
        const control_class_t&,
        const std::wstring& device_id, bool capture);
    media_stream_t create_stream(presentation_clock_t&&);
};

class stream_wasapi2 : public media_stream_clock_sink
{
    friend class source_wasapi2;
private:
    source_wasapi2_t source;
    time_unit drain_point;

    void on_stream_stop(time_unit);
public:
    explicit stream_wasapi2(const source_wasapi2_t&);

    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_wasapi2> stream_wasapi2_t;