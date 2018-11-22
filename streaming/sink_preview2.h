#pragma once
#include "async_callback.h"
#include "media_sink.h"
#include "media_stream.h"
#include "transform_videoprocessor2.h"
#include <Windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <memory>
#include <queue>
#include <mutex>
#include <atomic>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Dxgi.lib")

class sink_preview2 : public media_sink
{
    friend class stream_preview2;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;

    static const UINT32 padding_width = 20;
    static const UINT32 padding_height = 20;
private:
    context_mutex_t context_mutex;
    std::recursive_mutex d2d1_context_mutex;

    std::atomic_bool render;
    stream_videoprocessor2_controller_t size_box;

    HWND hwnd;
    UINT width, height;
    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<ID2D1HwndRenderTarget> rendertarget;
    CComPtr<ID2D1Device> d2d1dev;
    CComPtr<ID2D1DeviceContext> d2d1devctx;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<IDXGISwapChain1> swapchain;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<IDXGIOutput> dxgioutput;
    CComPtr<ID3D11DeviceContext> d3d11devctx;
    CComPtr<ID3D11RenderTargetView> render_target_view;
    CComPtr<ID2D1SolidColorBrush> box_brush;
    CComPtr<ID2D1StrokeStyle1> stroke_style;

    void draw_sample(const media_sample& sample_view, request_packet& rp);
public:
    sink_preview2(const media_session_t& session, context_mutex_t context_mutex);

    // initializes the window
    void initialize(
        HWND, 
        const CComPtr<ID2D1Device>&,
        const CComPtr<ID3D11Device>&, 
        const CComPtr<ID2D1Factory1>&);
    media_stream_t create_stream();

    void set_state(bool render) {this->render = render;}
    void set_size_box(const stream_videoprocessor2_controller_t& new_box);
    stream_videoprocessor2_controller_t get_size_box() const {return std::atomic_load(&this->size_box);}
    void update_size();
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
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_preview2> stream_preview2_t;