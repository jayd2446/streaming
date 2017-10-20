#pragma once
#include "async_callback.h"
#include "media_sink.h"
#include "media_stream.h"
#include <Windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <memory>
#include <queue>
#include <mutex>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Dxgi.lib")

// sink preview must be multithread aware

class sink_preview2 : public media_sink
{
    friend class stream_preview2;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    std::recursive_mutex samples_mutex;
    std::recursive_mutex& context_mutex;

    HWND hwnd;
    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<ID2D1HwndRenderTarget> rendertarget;
    CComPtr<ID2D1Device> d2d1dev;
    CComPtr<ID2D1DeviceContext> d2d1devctx;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<IDXGISwapChain1> swapchain;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<IDXGIOutput> dxgioutput;
    CComPtr<ID2D1Bitmap1> d2dtarget_bitmap;
    CComPtr<ID3D11DeviceContext> d3d11devctx;
    CComPtr<ID3D11RenderTargetView> render_target_view;

    void draw_sample(const media_sample_view_t& sample_view, request_packet& rp);
public:
    sink_preview2(const media_session_t& session, std::recursive_mutex& context_mutex);

    // initializes the window
    void initialize(
        UINT32 window_width, UINT32 window_height,
        HWND, 
        CComPtr<ID3D11Device>&, 
        CComPtr<ID3D11DeviceContext>&,
        CComPtr<IDXGISwapChain>&);
    media_stream_t create_stream();
};

typedef std::shared_ptr<sink_preview2> sink_preview2_t;

class stream_preview2 : public media_stream
{
private:
    sink_preview2_t sink;
public:
    explicit stream_preview2(const sink_preview2_t& sink);

    result_t request_sample(request_packet&, const media_stream*);
    // called by media session
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_preview2> stream_preview2_t;