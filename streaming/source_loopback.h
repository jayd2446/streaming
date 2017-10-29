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

class source_loopback : public media_source
{
    friend class stream_loopback;
public:
    typedef async_callback<source_loopback> async_callback_t;
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef request_queue<WORKER_STREAMS> request_queue_t;
    typedef request_queue_t::request_t request_t;
private:
    CComPtr<IAudioClient> audio_client;
    CComPtr<IAudioCaptureClient> audio_capture_client;
    CComPtr<async_callback_t> process_callback;
    CHandle process_event;
    MFWORKITEM_KEY callback_key;

    // one sample might contain multiple pcm samples
    std::recursive_mutex samples_mutex;
    std::deque<CComPtr<IMFSample>> samples;

    std::atomic_bool started;

    request_queue_t requests;

    // maximum number of audio frames(aka pcm samples) the allocated buffer can hold
    UINT32 samples_size;
    DWORD mf_alignment;
    UINT32 block_align, samples_per_second, channels;

    HRESULT copy_aligned(BYTE* to, const BYTE* from) const;
    HRESULT add_event_to_wait_queue();
    HRESULT create_waveformat_type(WAVEFORMATEX*);
    void process_cb(void*);
public:
    CComPtr<IMFMediaType> waveformat_type;

    explicit source_loopback(const media_session_t& session);
    ~source_loopback();

    HRESULT initialize();
    HRESULT start();

    media_stream_t create_stream(presentation_clock_t&);
};

typedef std::shared_ptr<source_loopback> source_loopback_t;

class stream_loopback : public media_stream, public presentation_clock_sink
{
public:
    typedef async_callback<stream_loopback> async_callback_t;
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    source_loopback_t source;
    media_sample_t sample;
    CComPtr<async_callback_t> process_callback;

    // presentation_clock_sink
    bool on_clock_start(time_unit);
    void on_clock_stop(time_unit);

    void process_cb(void*);
public:
    explicit stream_loopback(const source_loopback_t& source);

    bool get_clock(presentation_clock_t& c) {return this->source->session->get_current_clock(c);}

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_loopback> stream_loopback_t;