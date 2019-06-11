#pragma once
#include "async_callback.h"
#include "media_sink.h"
#include "media_stream.h"
#include "transform_videomixer.h"
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

class control_pipeline2;
typedef std::shared_ptr<control_pipeline2> control_pipeline2_t;

class sink_preview2 : public media_sink
{
    friend class stream_preview2;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;

    static const UINT32 padding_width = 20;
    static const UINT32 padding_height = 20;
private:
    control_pipeline2_t ctrl_pipeline;
    context_mutex_t context_mutex;
    mutable std::recursive_mutex d2d1_context_mutex, size_mutex;
    FLOAT size_point_radius;
    std::atomic_bool render;

    media_buffer_texture_t last_buffer;

    HWND hwnd;
    UINT width, height;
    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<ID2D1HwndRenderTarget> rendertarget;
    CComPtr<ID2D1Device> d2d1dev;
    CComPtr<ID2D1DeviceContext> d2d1devctx;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<IDXGISwapChain1> swapchain;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<ID3D11DeviceContext> d3d11devctx;
    CComPtr<ID3D11RenderTargetView> render_target_view;
    CComPtr<ID2D1SolidColorBrush> box_brush;
    CComPtr<ID2D1SolidColorBrush> line_brush;
    CComPtr<ID2D1SolidColorBrush> highlighted_brush;
    CComPtr<ID2D1StrokeStyle1> stroke_style;

    void draw_sample(const media_component_args*);
public:
    sink_preview2(const media_session_t& session, context_mutex_t context_mutex);

    // initializes the window
    void initialize(
        const control_pipeline2_t& ctrl_pipeline,
        HWND, 
        const CComPtr<ID2D1Device>&,
        const CComPtr<ID3D11Device>&, 
        const CComPtr<ID2D1Factory1>&);
    media_stream_t create_stream();

    // in preview window coordinates
    D2D1_RECT_F get_preview_rect() const;
    void get_window_size(UINT& width, UINT& height) const;
    // the radius is in preview window coordinates
    FLOAT get_size_point_radius() const {return this->size_point_radius;}

    void set_state(bool render) {this->render = render;}
    void update_size();
};

typedef std::shared_ptr<sink_preview2> sink_preview2_t;

class stream_preview2 : public media_stream
{
private:
    sink_preview2_t sink;
public:
    explicit stream_preview2(const sink_preview2_t& sink);

    result_t request_sample(const request_packet&, const media_stream*);
    // called by media session
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_preview2> stream_preview2_t;