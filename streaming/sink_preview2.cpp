#include "sink_preview2.h"
#include "assert.h"

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

#undef max
#undef min

sink_preview2::sink_preview2(const media_session_t& session, context_mutex_t context_mutex) : 
    media_sink(session), context_mutex(context_mutex), render(true)
{
}

void sink_preview2::initialize(
    HWND hwnd, 
    const CComPtr<ID2D1Device>& d2d1dev,
    const CComPtr<ID3D11Device>& d3d11dev,
    const CComPtr<ID2D1Factory1>& d2d1factory)
{
    this->hwnd = hwnd;
    this->d3d11dev = d3d11dev;
    this->d2d1factory = d2d1factory;
    this->d2d1dev = d2d1dev;

    HRESULT hr = S_OK;

    CComPtr<IDXGIAdapter> dxgiadapter;
    CComPtr<IDXGIFactory2> dxgifactory;
    CComPtr<ID3D11Texture2D> backbuffer;
    CComPtr<IDXGISurface> dxgibackbuffer;
    CComPtr<ID2D1Bitmap1> d2dtarget_bitmap;
    D2D1_BITMAP_PROPERTIES1 bitmap_props;
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {0};
    RECT r;

    std::lock(*this->context_mutex, this->d2d1_context_mutex);
    scoped_lock lock(*this->context_mutex, std::adopt_lock);
    scoped_lock lock2(this->d2d1_context_mutex, std::adopt_lock);

    // obtain the dxgi device of the d3d11 device
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&this->dxgidev));

    //// obtain the direct2d device
    //CHECK_HR(hr = this->d2d1factory->CreateDevice(this->dxgidev, &this->d2d1dev));

    // create a direct2d device context
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
    swapchain_desc.Flags = 0/*DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE*/;

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

    // create the brush for the sizing box
    CHECK_HR(hr = this->d2d1devctx->CreateSolidColorBrush(
        D2D1::ColorF(D2D1::ColorF::Red),
        &this->box_brush));

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

void sink_preview2::draw_sample(const media_sample& sample_view_, request_packet&)
{
    if(!this->render)
        return;

    HRESULT hr = S_OK;

    const media_sample_texture& sample_view =
        reinterpret_cast<const media_sample_texture&>(sample_view_);
    stream_videoprocessor2_controller_t size_box = std::atomic_load(&this->size_box);
    stream_videoprocessor2_controller::params_t params = {0};
    if(size_box)
        size_box->get_params(params);

    CComPtr<ID3D11Texture2D> texture = sample_view.buffer->texture;
    if(texture)
    {
        using namespace D2D1;
        scoped_lock lock(this->d2d1_context_mutex);

        const FLOAT canvas_w = (FLOAT)transform_videoprocessor2::canvas_width;
        const FLOAT canvas_h = (FLOAT)transform_videoprocessor2::canvas_height;
        const FLOAT preview_w = (FLOAT)(this->width - this->padding_width * 2);
        const FLOAT preview_h = (FLOAT)(this->height - this->padding_height * 2);
        bool invert;

        FLOAT canvas_scale = preview_w / canvas_w;
        FLOAT preview_x = (FLOAT)sink_preview2::padding_width, 
            preview_y = (FLOAT)sink_preview2::padding_height;
        if((canvas_scale * canvas_h) > preview_h)
        {
            canvas_scale = preview_h / canvas_h;
            preview_x = ((FLOAT)this->width - canvas_w * canvas_scale) / 2.f;
        }
        else
            preview_y = ((FLOAT)this->height - canvas_h * canvas_scale) / 2.f;

        Matrix3x2F canvas_to_preview = Matrix3x2F::Scale(canvas_w, canvas_h);
        invert = canvas_to_preview.Invert();
        canvas_to_preview = canvas_to_preview * 
            Matrix3x2F::Scale(canvas_w * canvas_scale, canvas_h * canvas_scale);
        canvas_to_preview = canvas_to_preview * Matrix3x2F::Translation(preview_x, preview_y);

        D2D1_ANTIALIAS_MODE old_mode;
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

        if(preview_x > std::numeric_limits<FLOAT>::epsilon() &&
            preview_y > std::numeric_limits<FLOAT>::epsilon())
        {
            this->d2d1devctx->SetTransform(canvas_to_preview);
            this->d2d1devctx->DrawBitmap(bitmap, RectF(0.f, 0.f, canvas_w, canvas_h));

            if(size_box)
            {
                Matrix3x2F dest = 
                    Matrix3x2F::Scale(
                        params.dest_rect.right - params.dest_rect.left,
                        params.dest_rect.bottom - params.dest_rect.top) *
                    Matrix3x2F::Translation(params.dest_rect.left, params.dest_rect.top) *
                    params.dest_m;
                this->d2d1devctx->SetTransform(dest * canvas_to_preview);

                old_mode = this->d2d1devctx->GetAntialiasMode();
                /*this->d2d1devctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);*/
                this->d2d1devctx->DrawRectangle(RectF(0.f, 0.f, 1.f, 1.f)
                    /*params.dest_rect*/, this->box_brush, 1.5f, this->stroke_style);
                this->d2d1devctx->SetAntialiasMode(old_mode);
            }
        }

        CHECK_HR(hr = this->d2d1devctx->EndDraw());

        // dxgi functions need to be synchronized with the context mutex
        {
            scoped_lock lock(*this->context_mutex);
            CHECK_HR(hr = this->swapchain->Present(0, 0));
        }
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

media_stream_t sink_preview2::create_stream()
{
    return stream_preview2_t(new stream_preview2(this->shared_from_this<sink_preview2>()));
}

void sink_preview2::set_size_box(const stream_videoprocessor2_controller_t& new_box)
{
    std::atomic_exchange(&this->size_box, new_box);
}

void sink_preview2::update_size()
{
    std::lock(*this->context_mutex, this->d2d1_context_mutex);
    scoped_lock lock(*this->context_mutex, std::adopt_lock);
    scoped_lock lock2(this->d2d1_context_mutex, std::adopt_lock);

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
        throw HR_EXCEPTION(hr);
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