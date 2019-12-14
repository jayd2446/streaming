#include "sink_preview2.h"
#include "assert.h"
#include "control_pipeline.h"
#include "control_video.h"
#include "gui_previewwnd.h"
#include <Windows.h>

//#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
void CHECK_HR(HRESULT hr)
{
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

#undef max
#undef min

sink_preview2::sink_preview2(const media_session_t& session) : media_sink(session)
{
}

void sink_preview2::initialize(
    HWND preview_wnd,
    const CComPtr<ID3D11Device>& d3d11dev,
    const CComPtr<ID2D1Factory1>& d2d1factory,
    const CComPtr<ID2D1Device>& d2d1dev,
    std::recursive_mutex& context_mutex)
{
    this->d3d11dev = d3d11dev;
    this->d2d1factory = d2d1factory;
    this->d2d1dev = d2d1dev;

    // initialize the preview
    HRESULT hr = S_OK;

    CComPtr<IDXGIAdapter> dxgiadapter;
    CComPtr<IDXGIFactory2> dxgifactory;
    CComPtr<ID3D11Texture2D> backbuffer;
    CComPtr<IDXGISurface> dxgibackbuffer;
    CComPtr<ID2D1Bitmap1> d2dtarget_bitmap;
    D2D1_BITMAP_PROPERTIES1 bitmap_props;
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {0};

    std::lock(context_mutex, this->d2d1_context_mutex);
    scoped_lock lock(context_mutex, std::adopt_lock);
    scoped_lock lock2(this->d2d1_context_mutex, std::adopt_lock);

    // obtain the dxgi device of the d3d11 device
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&this->dxgidev));

    // create a direct2d device context
    CHECK_HR(hr = this->d2d1dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &this->d2d1devctx));

    // create swap chain for the hwnd
    swapchain_desc.Width = swapchain_desc.Height = 0; // use automatic sizing
    // this is the most common swapchain
    swapchain_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapchain_desc.Stereo = false;
    swapchain_desc.SampleDesc.Count = 1; // dont use multisampling
    swapchain_desc.SampleDesc.Quality = 0;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = 2; // use double buffering to enable flip
    swapchain_desc.Scaling = DXGI_SCALING_NONE;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapchain_desc.Flags = 0/*DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE*/;

    // identify the physical adapter (gpu or card) this device runs on
    CHECK_HR(hr = this->dxgidev->GetAdapter(&dxgiadapter));

    // get the factory object that created this dxgi device
    CHECK_HR(hr = dxgiadapter->GetParent(IID_PPV_ARGS(&dxgifactory)));

    // get the final swap chain for this window from the dxgi factory
    CHECK_HR(hr = dxgifactory->CreateSwapChainForHwnd(
        this->d3d11dev, preview_wnd, &swapchain_desc, nullptr, nullptr, &this->swapchain));

    // ensure that dxgi doesn't queue more than one frame at a time
    /*hr = this->dxgidev->SetMaximumFrameLatency(1);*/

    // get the backbuffer for this window which is the final 3d render target
    CHECK_HR(hr = this->swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)));

    // now we set up the direct2d render target bitmap linked to the swapchain
    // whenever we render to this bitmap, it is directly rendered to the swap chain associated
    // with this window
    bitmap_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    // direct2d needs the dxgi version of the backbuffer surface pointer
    CHECK_HR(hr = this->swapchain->GetBuffer(0, IID_PPV_ARGS(&dxgibackbuffer)));

    // get the d2d surface from the dxgi back buffer to use as the d2d render target
    CHECK_HR(hr = this->d2d1devctx->CreateBitmapFromDxgiSurface(
        dxgibackbuffer, &bitmap_props, &d2dtarget_bitmap));

    // now we can set the direct2d render target
    this->d2d1devctx->SetTarget(d2dtarget_bitmap);

    // create the brushes for sizing
    CHECK_HR(hr = this->d2d1devctx->CreateSolidColorBrush(
        D2D1::ColorF(D2D1::ColorF::Red),
        &this->box_brush));
    CHECK_HR(hr = this->d2d1devctx->CreateSolidColorBrush(
        D2D1::ColorF(D2D1::ColorF::LimeGreen),
        &this->line_brush));
    CHECK_HR(hr = this->d2d1devctx->CreateSolidColorBrush(
        D2D1::ColorF(D2D1::ColorF::Red),
        &this->highlighted_brush));

    // create the stroke style for the box
    {
        FLOAT dashes[] = {4.f, 4.f};
        D2D1_STROKE_STYLE_PROPERTIES1 stroke_props = D2D1::StrokeStyleProperties1();
        // world transform doesn't affect the stroke width
        stroke_props.transformType = D2D1_STROKE_TRANSFORM_TYPE_FIXED;
        stroke_props.dashStyle = D2D1_DASH_STYLE_CUSTOM;
        /*stroke_props.dashStyle = D2D1_DASH_STYLE_DASH;*/
        CHECK_HR(hr = this->d2d1factory->CreateStrokeStyle(
            stroke_props, dashes, ARRAYSIZE(dashes), &this->stroke_style));
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void sink_preview2::update_preview_sample(const media_component_args* args_)
{
    // videomixer outputs h264 encoder args;
    // sink preview just previews the last frame that is send to encoder;
    // it is assumed that there is at least one frame if the args isn't empty
    const media_component_h264_encoder_args* args =
        static_cast<const media_component_h264_encoder_args*>(args_);

    if(args)
    {
        // find the first available bitmap
        for(const auto& item : args->sample->get_frames())
        {
            if(item.buffer && item.buffer->bitmap)
            {
                std::atomic_store(&this->last_buffer, item.buffer);
                break;
            }
        }
    }
}

media_stream_t sink_preview2::create_stream()
{
    return stream_preview2_t(new stream_preview2(this->shared_from_this<sink_preview2>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_preview2::stream_preview2(const sink_preview2_t& sink) : sink(sink)
{
}

media_stream::result_t stream_preview2::request_sample(const request_packet& rp, const media_stream*)
{
    return this->sink->session->request_sample(this, rp) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_preview2::process_sample(
    const media_component_args* args, const request_packet& rp, const media_stream*)
{
    this->sink->update_preview_sample(args);
    return this->sink->session->give_sample(this, args, rp) ? OK : FATAL_ERROR;
}
