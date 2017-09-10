#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include <Windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <atlbase.h>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Dxgi.lib")

class stream_preview;

class sink_preview : public media_sink
{
    friend class stream_preview;
private:
    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<ID2D1HwndRenderTarget> rendertarget;
    CComPtr<ID2D1Device> d2d1dev;
    CComPtr<ID2D1DeviceContext> d2d1devctx;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<IDXGISwapChain1> swapchain;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<ID2D1Bitmap1> d2dtarget_bitmap;
    bool drawn;

    CComPtr<ID3D11DeviceContext> d3d11devctx;
    CComPtr<ID3D11RenderTargetView> render_target_view;
    HWND hwnd;
public:
    explicit sink_preview(const media_session_t& session);

    media_stream_t create_stream(presentation_clock_t& clock);

    // (presentation clock can be accessed from media session)
    // set_presentation_clock

    // initializes the window
    void initialize(
        UINT32 window_width, UINT32 window_height,
        HWND, 
        CComPtr<ID3D11Device>&, 
        CComPtr<ID3D11DeviceContext>&,
        CComPtr<IDXGISwapChain>&);

    //// begin requesting samples
    //bool start(media_stream&);
};

typedef std::shared_ptr<sink_preview> sink_preview_t;

class stream_preview : public media_stream, public presentation_clock_sink
{
private:
    sink_preview_t sink;
    time_unit pipeline_latency;
    time_unit show_time, request_time;
    time_unit start_time;

    bool on_clock_start(time_unit);
    void on_clock_stop(time_unit);
    void scheduled_callback(time_unit due_time);
public:
    stream_preview(const sink_preview_t& sink, presentation_clock_t& clock);
    ~stream_preview();

    bool get_clock(presentation_clock_t&);

    // called by sink_preview
    result_t request_sample();
    // called by media session
    result_t process_sample(const media_sample_t&);
};