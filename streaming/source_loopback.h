#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include "presentation_clock.h"
#include <Audioclient.h>
#include <mfapi.h>
#include <memory>
#include <deque>
#include <mutex>
#include <atomic>

#pragma comment(lib, "Mfplat.lib")

// requests are served roughly twice in a buffer duration

#define BUFFER_DURATION SECOND_IN_TIME_UNIT
#define MILLISECOND_IN_TIMEUNIT (SECOND_IN_TIME_UNIT / 1000)

class source_loopback;
typedef std::shared_ptr<source_loopback> source_loopback_t;

// timestamps are aligned

class source_loopback : public media_source
{
    friend class stream_loopback;
public:
    typedef async_callback<source_loopback> async_callback_t;
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef request_queue::request_t request_t;

    // the base of consecutive samples the relative sample refers to
    struct sample_base_t {LONGLONG time, sample;};

    // wasapi is always 32 bit float in shared mode
    typedef int16_t bit_depth_t;
private:
    CComPtr<IAudioClient> audio_client;
    CComPtr<IAudioCaptureClient> audio_capture_client;
    CComPtr<IAudioClock> audio_clock;
    CHandle process_event;
    MFWORKITEM_KEY callback_key;
    DWORD work_queue_id;

    std::recursive_mutex process_mutex;
    CComPtr<async_callback_t> process_callback;
    std::recursive_mutex serve_mutex;
    CComPtr<async_callback_t> serve_callback;

    std::recursive_mutex samples_mutex;
    std::deque<CComPtr<IMFSample>> samples;
    sample_base_t stream_base;
    frame_unit consumed_samples_end;
    UINT64 device_time_position;
    REFERENCE_TIME buffer_actual_duration;

    bool started;

    request_queue requests;

    // maximum number of audio frames(aka pcm samples) the allocated buffer can hold
    UINT32 block_align, samples_per_second, channels;

    HRESULT add_event_to_wait_queue();
    HRESULT create_waveformat_type(WAVEFORMATEX*);
    void process_cb(void*);
    void serve_cb(void*);
    void serve_requests();
    HRESULT start();

    double sine_var;
public:
    bool generate_sine;
    CComPtr<IMFMediaType> waveformat_type;

    explicit source_loopback(const media_session_t& session);
    ~source_loopback();

    HRESULT initialize(bool capture = false);
    media_stream_t create_stream();

    void convert_32bit_float_to_bitdepth_pcm(
        UINT32 frames, UINT32 channels,
        const float* in, bit_depth_t* out, bool silent);

    // returns the block align of the converted samples in the buffer
    UINT32 get_block_align() const {return sizeof(bit_depth_t) * this->channels;}
};

class stream_loopback : public media_stream
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    source_loopback_t source;
public:
    explicit stream_loopback(const source_loopback_t& source);

    bool get_clock(presentation_clock_t& c) {return this->source->session->get_current_clock(c);}

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_loopback> stream_loopback_t;