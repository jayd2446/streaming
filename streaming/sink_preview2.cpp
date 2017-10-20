#include "sink_preview2.h"
#include <cassert>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

sink_preview2::sink_preview2(const media_session_t& session, std::recursive_mutex& context_mutex) : 
    media_sink(session), context_mutex(context_mutex)
{
}

void sink_preview2::initialize(
    UINT32 window_width, UINT32 window_height,
    HWND hwnd, 
    CComPtr<ID3D11Device>& d3d11dev, 
    CComPtr<ID3D11DeviceContext>& d3d11devctx,
    CComPtr<IDXGISwapChain>& swapchain)
{
    this->hwnd = hwnd;
    this->d3d11dev = d3d11dev;

    HRESULT hr = S_OK;

    CComPtr<IDXGIAdapter> dxgiadapter;
    CComPtr<IDXGIFactory2> dxgifactory;
    CComPtr<ID3D11Texture2D> backbuffer;
    CComPtr<IDXGISurface> dxgibackbuffer;
    D2D1_BITMAP_PROPERTIES1 bitmap_props;
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {0};

    // create d2d factory
    CHECK_HR(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &this->d2d1factory));

    // obtain the dxgi device of the d3d11 device
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&this->dxgidev));

    // obtain the direct2d device
    CHECK_HR(hr = this->d2d1factory->CreateDevice(this->dxgidev, &this->d2d1dev));

    // get the direct2d device context
    CHECK_HR(hr = this->d2d1dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &this->d2d1devctx));

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
    swapchain_desc.Flags = 0;

    // identify the physical adapter (gpu or card) this device runs on
    CHECK_HR(hr = this->dxgidev->GetAdapter(&dxgiadapter));
    CHECK_HR(hr = dxgiadapter->EnumOutputs(0, &this->dxgioutput));

    // get the factory object that created this dxgi device
    CHECK_HR(hr = dxgiadapter->GetParent(IID_PPV_ARGS(&dxgifactory)));

    // get the final swap chain for this window from the dxgi factory
    CHECK_HR(hr = dxgifactory->CreateSwapChainForHwnd(
        this->d3d11dev, hwnd, &swapchain_desc, NULL, NULL, &this->swapchain));

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
        dxgibackbuffer, &bitmap_props, &this->d2dtarget_bitmap));

    // now we can set the direct2d render target
    this->d2d1devctx->SetTarget(this->d2dtarget_bitmap);

done:
    if(FAILED(hr))
        throw std::exception();
}

void sink_preview2::draw_sample(const media_sample_view_t& sample_view, request_packet& rp)
{
    HRESULT hr = S_OK;

    CComPtr<ID3D11Texture2D> texture = sample_view->get_sample<media_sample_texture>()->texture;
    if(texture)
    {
        CComPtr<ID2D1Bitmap1> bitmap;
        CComPtr<IDXGISurface> surface;

        scoped_lock lock(this->context_mutex);
        CHECK_HR(hr = texture->QueryInterface(&surface));
        CHECK_HR(hr = this->d2d1devctx->CreateBitmapFromDxgiSurface(
            surface,
            D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
            &bitmap));

        this->d2d1devctx->BeginDraw();
        this->d2d1devctx->DrawBitmap(bitmap);
        this->d2d1devctx->EndDraw();

        // dxgi functions need to be synchronized with the context mutex
        this->swapchain->Present(0, 0);
    }

done:
    if(FAILED(hr))
        throw std::exception();
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

media_stream::result_t stream_preview2::request_sample(request_packet& rp, const media_stream*)
{
    assert(false);
    return OK;
}

media_stream::result_t stream_preview2::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    this->sink->draw_sample(sample_view, rp);
    return OK;
}