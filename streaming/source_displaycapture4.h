#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "media_sample.h"
#include "presentation_clock.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <memory>
#include <deque>
#include <queue>
#include <mutex>

#pragma comment(lib, "dxgi")

#define SAMPLE_DEPTH 6
#define FPS60_INTERVAL 166667

// TODO: source capturing threads should have a priority of Capture class;
// sinks have the playback priority which is a bit lower

/*

sources are responsible for giving the most accurate sample for the given request time;
sources must not drop sample requests;

request time is used to identify separate sample requests

*/

class source_displaycapture4;
typedef std::shared_ptr<source_displaycapture4> source_displaycapture4_t;
class stream_displaycapture4;

struct stream_displaycapture4_cb
{
    stream_displaycapture4* cb;
    time_unit device_request_time, request_time;
};

// TODO: frame capturing should be timed so that it begins at the middle of the
// refresh rate
class source_displaycapture4 : public media_source
{
    friend class stream_preview;
    friend class stream_displaycapture4;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;

    // captures a new frame in exact intervals
    struct thread_capture : public presentation_clock_sink
    {
        std::weak_ptr<source_displaycapture4> source;
        presentation_clock_t real_time_clock;
        bool running;
        AsyncCallback<thread_capture> callback;
        DWORD work_queue;
        time_unit due_time;

        thread_capture(source_displaycapture4_t& source, presentation_clock_t&);
        ~thread_capture();
        bool on_clock_start(time_unit);
        void on_clock_stop(time_unit);
        bool get_clock(presentation_clock_t&);
        void scheduled_callback(time_unit due_time);
        HRESULT capture_cb(IMFAsyncResult*);

        bool get_source(source_displaycapture4_t&);
        void schedule_new(time_unit due_time);
    };
    typedef CComPtr<thread_capture> thread_capture_t;
private:
    thread_capture_t capture_thread;
    presentation_clock_t capture_thread_clock;

    CComPtr<IDXGIOutputDuplication> output_duplication;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> d3d11devctx;

    // sample history
    int current_frame;
    CComPtr<ID3D11Texture2D> screen_frame[SAMPLE_DEPTH];
    media_sample_t samples[SAMPLE_DEPTH];
    std::recursive_mutex mutex;

    void capture_frame(LARGE_INTEGER start_time);
public:
    explicit source_displaycapture4(const media_session_t& session);

    media_stream_t create_stream();
    // after initializing starts the capturing
    // TODO: remove these unused parameters
    HRESULT initialize(ID3D11Device*, ID3D11DeviceContext*);
    // returns a frame closest to the requested time or NULL
    media_sample_t capture_frame(time_unit timestamp, bool& too_new);

    // returns a valid clock
    presentation_clock_t get_device_clock();
};

// TODO: use request packets that contain the request_time and additionally the packet number;
// these request packets travel from sink to source to sink and optionally contain sample data
// TODO: use weak pointer for source
class stream_displaycapture4 : public media_stream
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    source_displaycapture4_t source;
public:
    explicit stream_displaycapture4(const source_displaycapture4_t& source);

    bool get_clock(presentation_clock_t& c) {return this->source->session->get_current_clock(c);}
    // called by media session
    result_t request_sample(time_unit request_time);
    // called by source_displaycapture
    result_t process_sample(const media_sample_t&, time_unit request_time);
};