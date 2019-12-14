#pragma once
#include "async_callback.h"
#include "media_sink.h"
#include "media_stream.h"
#include "transform_videomixer.h"
#include <d3d11.h>
#include <d2d1_1.h>
#include <atlbase.h>
#include <memory>
#include <atomic>
#include <mutex>

class sink_preview2 : public media_sink
{
    friend class stream_preview2;
public:
    using scoped_lock = std::lock_guard<std::recursive_mutex>;
private:
    media_buffer_texture_t last_buffer;

    void update_preview_sample(const media_component_args*);
public:
    std::recursive_mutex d2d1_context_mutex;

    // immutable
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<ID2D1Device> d2d1dev;
    CComPtr<ID2D1DeviceContext> d2d1devctx;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<IDXGISwapChain1> swapchain;
    CComPtr<ID2D1SolidColorBrush> box_brush;
    CComPtr<ID2D1SolidColorBrush> line_brush;
    CComPtr<ID2D1SolidColorBrush> highlighted_brush;
    CComPtr<ID2D1StrokeStyle1> stroke_style;

    explicit sink_preview2(const media_session_t& session);

    void initialize(
        HWND preview_wnd,
        const CComPtr<ID3D11Device>&,
        const CComPtr<ID2D1Factory1>&,
        const CComPtr<ID2D1Device>&,
        std::recursive_mutex& context_mutex);
    media_stream_t create_stream();

    media_buffer_texture_t get_last_buffer() const { return std::atomic_load(&this->last_buffer); }
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