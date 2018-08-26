#include "sink_preview2.h"
#include "assert.h"

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

#undef max
#undef min

sink_preview2::sink_preview2(const media_session_t& session, context_mutex_t context_mutex) : 
    media_sink(session), context_mutex(context_mutex)
{
}

void sink_preview2::initialize(
    HWND hwnd, 
    CComPtr<ID3D11Device>& d3d11dev)
{
    this->hwnd = hwnd;
    this->d3d11dev = d3d11dev;

    HRESULT hr = S_OK;

    CComPtr<IDXGIAdapter> dxgiadapter;
    CComPtr<IDXGIFactory2> dxgifactory;
    CComPtr<ID3D11Texture2D> backbuffer;
    CComPtr<IDXGISurface> dxgibackbuffer;
    CComPtr<ID2D1Bitmap1> d2dtarget_bitmap;
    D2D1_BITMAP_PROPERTIES1 bitmap_props;
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {0};
    RECT r;

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
    swapchain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;

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
        dxgibackbuffer, &bitmap_props, &d2dtarget_bitmap));

    // now we can set the direct2d render target
    this->d2d1devctx->SetTarget(d2dtarget_bitmap);

    // set the size
    GetClientRect(this->hwnd, &r);
    this->width = std::abs(r.right - r.left);
    this->height = std::abs(r.bottom - r.top);

done:
    if(FAILED(hr))
        throw std::exception();
}

void sink_preview2::draw_sample(const media_sample& sample_view_, request_packet&)
{
    HRESULT hr = S_OK;

    const media_sample_texture& sample_view =
        reinterpret_cast<const media_sample_texture&>(sample_view_);

    CComPtr<ID3D11Texture2D> texture = sample_view.buffer->texture;
    if(texture)
    {
        using namespace D2D1;
        D3D11_TEXTURE2D_DESC desc;
        sample_view.buffer->texture->GetDesc(&desc);

        const FLOAT width = (FLOAT)this->width, 
            height = (FLOAT)this->height;
        const FLOAT scale_w = width / desc.Width,
            scale_h = height / desc.Height;
        const bool use_scale_w = scale_w < scale_h;
        const FLOAT scale = use_scale_w ? scale_w : scale_h;
        FLOAT padding = use_scale_w ? desc.Height * scale : desc.Width * scale;
        if(use_scale_w)
            padding = (height - padding) / 2;
        else
            padding = (width - padding) / 2;

        D2D1_RECT_F rect;
        rect.top = rect.left = 20 / scale;
        rect.right = (FLOAT)desc.Width - 20 / scale;
        rect.bottom = (FLOAT)desc.Height - 20 / scale;

        Matrix3x2F scale_mtx = Matrix3x2F::Scale(scale, scale);
        Matrix3x2F translation_mtx = use_scale_w ?
            Matrix3x2F::Translation(0.f, padding) :
            Matrix3x2F::Translation(padding, 0.f);
        
        scoped_lock lock(*this->context_mutex);
        CComPtr<ID2D1Bitmap1> bitmap;
        CComPtr<IDXGISurface> surface;
        CHECK_HR(hr = texture->QueryInterface(&surface));
        CHECK_HR(hr = this->d2d1devctx->CreateBitmapFromDxgiSurface(
            surface,
            BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
            &bitmap));
        this->d2d1devctx->BeginDraw();
        this->d2d1devctx->Clear(ColorF(ColorF::DimGray));
        if(rect.top < rect.bottom && rect.left < rect.right)
        {
            this->d2d1devctx->SetTransform(scale_mtx * translation_mtx);
            this->d2d1devctx->DrawBitmap(bitmap, &rect);
        }
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

void sink_preview2::update_size()
{
    scoped_lock lock(*this->context_mutex);

    RECT r;
    GetClientRect(this->hwnd, &r);
    this->width = std::abs(r.right - r.left);
    this->height = std::abs(r.bottom - r.top);

    HRESULT hr = S_OK;
    CComPtr<IDXGISurface> dxgibackbuffer;
    CComPtr<ID2D1Bitmap1> d2dtarget_bitmap;
    D2D1_BITMAP_PROPERTIES1 bitmap_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    // reset the device context target
    this->d2d1devctx->SetTarget(NULL);
    CHECK_HR(hr = this->swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
    // direct2d needs the dxgi version of the backbuffer surface pointer
    CHECK_HR(hr = this->swapchain->GetBuffer(0, IID_PPV_ARGS(&dxgibackbuffer)));
    // get the d2d surface from the dxgi back buffer to use as the d2d render target
    CHECK_HR(hr = this->d2d1devctx->CreateBitmapFromDxgiSurface(
        dxgibackbuffer, &bitmap_props, &d2dtarget_bitmap));

    // now we can set the direct2d render target
    this->d2d1devctx->SetTarget(d2dtarget_bitmap);

done:
    if(FAILED(hr))
        throw std::exception();
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_preview2::stream_preview2(const sink_preview2_t& sink) : sink(sink)
{
}

media_stream::result_t stream_preview2::request_sample(request_packet& rp, const media_stream*)
{
    return this->sink->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_preview2::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream*)
{
    this->sink->draw_sample(sample_view, rp);
    return this->sink->session->give_sample(this, sample_view, rp, false) ? OK : FATAL_ERROR;
}