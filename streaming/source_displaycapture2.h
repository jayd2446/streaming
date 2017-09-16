#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "media_sample.h"
#include "presentation_clock.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <memory>

#pragma comment(lib, "dxgi")

// display capturing must work at the same frequency as the source

class source_displaycapture2 : public media_source
{
    friend class stream_preview;
    friend class stream_displaycapture2;
private:
    CComPtr<IDXGIOutputDuplication> output_duplication;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> d3d11devctx;
    volatile int active_frame, buffered_frame;
    volatile bool new_available;
    CComPtr<ID3D11Texture2D> screen_frame[2];
    HANDLE screen_frame_handle[2];

    LARGE_INTEGER start_time;
public:
    explicit source_displaycapture2(const media_session_t& session);

    media_stream_t create_stream(presentation_clock_t& clock);
    HRESULT initialize(CComPtr<ID3D11Device>&, CComPtr<ID3D11DeviceContext>&);
    void capture_frame();

    HANDLE give_texture();
    void give_back_texture();
};

typedef std::shared_ptr<source_displaycapture2> source_displaycapture2_t;

class stream_displaycapture2 : public media_stream, public presentation_clock_sink
{
    friend DWORD WINAPI ThreadProc(LPVOID parameter);
private:
    AsyncCallback<stream_displaycapture2> callback;
    source_displaycapture2_t source;
    bool running;
    DWORD work_queue;
    HANDLE signal;

    bool on_clock_start(time_unit);
    void on_clock_stop(time_unit);
    void scheduled_callback(time_unit due_time);
    HRESULT capture_cb(IMFAsyncResult*);
public:
    stream_displaycapture2(const source_displaycapture2_t& source, presentation_clock_t& clock);
    ~stream_displaycapture2();
    bool get_clock(presentation_clock_t& c) {return this->source->session->get_current_clock(c);}

    // called by media session
    result_t request_sample();
    // called by source_displaycapture
    result_t process_sample(const media_sample_t&);
};