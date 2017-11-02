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

#define THREAD_PERIODICITY (SECOND_IN_TIME_UNIT / 2)
#define BUFFER_LEN (THREAD_PERIODICITY * 2)

class source_loopback;
typedef std::shared_ptr<source_loopback> source_loopback_t;

class source_loopback : public media_source
{
    friend class stream_loopback;
public:
    typedef async_callback<source_loopback> async_callback_t;
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef request_queue::request_t request_t;

    struct thread_capture : public presentation_clock_sink
    {
        typedef async_callback<thread_capture> async_callback_t;

        std::recursive_mutex samples_mutex, serve_mutex;
        // one packet(=sample) contains one consecutive buffer of audio frames
        std::deque<CComPtr<IMFSample>> samples;

        std::weak_ptr<source_loopback> source;
        CComPtr<async_callback_t> callback;
        volatile bool running;
        // time position is used to check capture buffer's time position
        UINT64 device_time_position;

        explicit thread_capture(source_loopback_t& source);

        bool on_clock_start(time_unit);
        void on_clock_stop(time_unit);
        bool get_clock(presentation_clock_t&);
        void scheduled_callback(time_unit);

        void capture_buffer(const source_loopback_t&);
        void serve_requests();

        void serve_cb(void*);

        bool get_source(source_loopback_t&);
        void schedule_new(time_unit);
    };
    typedef std::shared_ptr<thread_capture> thread_capture_t;
private:
    thread_capture_t capture_thread;
    presentation_time_source_t capture_thread_time;
    presentation_clock_t capture_thread_clock;

    CComPtr<IAudioClient> audio_client;
    CComPtr<IAudioCaptureClient> audio_capture_client;
    CComPtr<IAudioClock> audio_clock;
    std::recursive_mutex process_mutex;
    CComPtr<async_callback_t> process_callback;
    std::recursive_mutex serve_mutex;
    CComPtr<async_callback_t> serve_callback;
    CHandle process_event;
    MFWORKITEM_KEY callback_key;

    UINT64 device_time_position;

    // one sample might contain multiple pcm samples
    std::recursive_mutex samples_mutex;
    std::deque<CComPtr<IMFSample>> samples;
    struct sample_base_t
    {
        LONGLONG samples_timebase, samples_samplebase;
    };
    sample_base_t samples_base;

    std::atomic_bool started;

    request_queue requests;

    // maximum number of audio frames(aka pcm samples) the allocated buffer can hold
    UINT32 samples_size;
    DWORD mf_alignment;
    UINT32 block_align, samples_per_second, channels;

    HRESULT copy_aligned(BYTE* to, const BYTE* from) const;
    HRESULT add_event_to_wait_queue();
    HRESULT create_waveformat_type(WAVEFORMATEX*);
    void process_cb(void*);
    void serve_cb(void*);
    HRESULT start();
    presentation_clock_t get_device_clock();

    void pull_buffer(media_sample_view_t&, const request_packet&);
    media_stream::result_t serve_requests();
public:
    CComPtr<IMFMediaType> waveformat_type;

    explicit source_loopback(const media_session_t& session);
    ~source_loopback();

    HRESULT initialize();
    media_stream_t create_stream(presentation_clock_t&);
};

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