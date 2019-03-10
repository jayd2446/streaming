#pragma once
#include "source_base.h"
#include "async_callback.h"
#include "transform_aac_encoder.h"
#include "transform_audiomixer2.h"
#include "audio_resampler.h"
#include <Audioclient.h>
#include <mfapi.h>

#pragma comment(lib, "Mfplat.lib")

#define CAPTURE_BUFFER_DURATION (SECOND_IN_TIME_UNIT) // 1s buffer

struct IMMDevice;

class source_wasapi : public source_base<media_component_audiomixer_args>
{
    friend class stream_wasapi;
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef async_callback<source_wasapi> async_callback_t;
    typedef buffer_pool<media_buffer_memory_pooled> buffer_pool_memory_t;
    typedef buffer_pool<media_sample_audio_mixer_frames_pooled> buffer_pool_audio_frames_t;

    // wasapi is always 32 bit float in shared mode
    typedef float bit_depth_t;
    static const INT64 capture_interval_ms = 40;
    static const frame_unit maximum_buffer_size = transform_aac_encoder::sample_rate;
private:
    control_class_t ctrl_pipeline;
    audio_resampler resampler;

    std::mutex captured_audio_mutex;
    std::shared_ptr<buffer_pool_memory_t> buffer_pool_memory;
    std::shared_ptr<buffer_pool_audio_frames_t> buffer_pool_audio_frames;
    media_sample_audio_mixer_frames_t captured_audio;
    frame_unit last_captured_frame_end;

    bool started, capture, in_wait_queue;

    frame_unit native_frame_base;
    frame_unit next_frame_position;
    bool set_new_frame_base;

    CComPtr<IAudioClient> audio_client, audio_client_render;
    CComPtr<IAudioCaptureClient> audio_capture_client;
    CComPtr<IAudioRenderClient> audio_render_client;

    CComPtr<IMFMediaType> waveformat_type;
    UINT32 block_align, samples_per_second, channels;
    REFERENCE_TIME buffer_actual_duration;
    UINT32 render_buffer_frame_count;
    UINT32 resampled_block_align;

    CComPtr<async_callback_t> capture_callback;
    MFWORKITEM_KEY capture_work_key;
    DWORD work_queue_id;

    // source_base
    stream_source_base_t create_derived_stream();
    bool get_samples_end(const request_t&, frame_unit& end);
    void make_request(request_t&, frame_unit frame_end);
    void dispatch(request_t&);

    void sine_wave(BYTE* data, DWORD len);
    double sine_wave_counter;

    HRESULT queue_new_capture();
    void capture_cb(void*);
    HRESULT play_silence();
    //  plays silence in loopback devices so that sample positions
    // stay consistent in regard to time
    HRESULT initialize_render(IMMDevice*, WAVEFORMATEX*);
    HRESULT create_waveformat_type(WAVEFORMATEX*);
public:
    explicit source_wasapi(const media_session_t& session);
    ~source_wasapi();

    void initialize(
        const control_class_t&,
        const std::wstring& device_id, bool capture);
};

typedef std::shared_ptr<source_wasapi> source_wasapi_t;

class stream_wasapi : public stream_source_base<source_base<media_component_audiomixer_args>>
{
private:
    source_wasapi_t source;
    void on_component_start(time_unit);
public:
    explicit stream_wasapi(const source_wasapi_t&);
};

typedef std::shared_ptr<stream_wasapi> stream_wasapi_t;