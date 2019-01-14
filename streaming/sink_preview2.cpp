#include "sink_preview2.h"
#include "assert.h"
#include "control_pipeline2.h"
#include "control_video2.h"
#define _USE_MATH_DEFINES
#include <math.h>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
#define SIZE_POINT_RADIUS 5.f

#undef max
#undef min

sink_preview2::sink_preview2(const media_session_t& session, context_mutex_t context_mutex) : 
    media_sink(session), context_mutex(context_mutex), render(true),
    size_point_radius(SIZE_POINT_RADIUS)
{
}

void sink_preview2::initialize(
    const control_pipeline2_t& ctrl_pipeline,
    HWND hwnd, 
    const CComPtr<ID2D1Device>& d2d1dev,
    const CComPtr<ID3D11Device>& d3d11dev,
    const CComPtr<ID2D1Factory1>& d2d1factory)
{
    this->ctrl_pipeline = ctrl_pipeline;
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

    std::lock(*this->context_mutex, this->d2d1_context_mutex, this->size_mutex);
    scoped_lock lock(*this->context_mutex, std::adopt_lock);
    scoped_lock lock2(this->d2d1_context_mutex, std::adopt_lock);
    scoped_lock lock3(this->size_mutex, std::adopt_lock);

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

void sink_preview2::draw_sample(const media_component_video_args_t& args, request_packet&)
{
    if(!this->render)
        return;

    HRESULT hr = S_OK;
    bool has_video_control = false;
    int highlighted_points;
    D2D1_RECT_F dest_rect;
    D2D1::Matrix3x2F dest_m;
    control_video2::video_params_t video_params;
    {
        control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
        if(this->ctrl_pipeline->selected_items.empty())
            goto out;
        control_video2* video_control = 
            dynamic_cast<control_video2*>(this->ctrl_pipeline->selected_items[0]);
        if(!video_control)
            goto out;

        has_video_control = true;
        highlighted_points = video_control->get_highlighted_points();
        dest_rect = video_control->get_rectangle(true);
        dest_m = video_control->get_transformation(true);
        video_params = video_control->get_video_params(true);
    }

out:
    // TODO: decide if the outlines are drawn even if the buffer is null;
    // the outlines should be drawn even when the sample is silent
    if(args && args->single_buffer)
    {
        using namespace D2D1;
        scoped_lock lock(this->d2d1_context_mutex);

        CComPtr<ID3D11Texture2D> texture = args->single_buffer->texture;
        D2D1_RECT_F preview_rect = this->get_preview_rect();
        bool invert;

        Matrix3x2F canvas_to_preview = 
            Matrix3x2F::Scale((FLOAT)transform_videomixer::canvas_width, 
            (FLOAT)transform_videomixer::canvas_height);
        invert = canvas_to_preview.Invert();
        canvas_to_preview = canvas_to_preview * 
            Matrix3x2F::Scale(preview_rect.right - preview_rect.left,
                preview_rect.bottom - preview_rect.top);
        canvas_to_preview = canvas_to_preview * Matrix3x2F::Translation(
            preview_rect.left, preview_rect.top);

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

        if(preview_rect.left > std::numeric_limits<FLOAT>::epsilon() &&
            preview_rect.top > std::numeric_limits<FLOAT>::epsilon())
        {
            // draw preview rect
            this->d2d1devctx->SetTransform(canvas_to_preview);
            this->d2d1devctx->DrawBitmap(bitmap, RectF(0.f, 0.f, 
                (FLOAT)transform_videomixer::canvas_width,
                (FLOAT)transform_videomixer::canvas_height));

            if(has_video_control)
            {
                /*control_video::video_params_t video_params = video_control->get_video_params(true);*/
                
                // draw size box
                FLOAT box_stroke = 1.5f;
                /*Matrix3x2F dest = 
                    Matrix3x2F::Scale(
                        video_params.rectangle.right - video_params.rectangle.left,
                        video_params.rectangle.bottom - video_params.rectangle.top) *
                    Matrix3x2F::Rotation(video_params.rotation) *
                    Matrix3x2F::Translation(video_params.rectangle.left, video_params.rectangle.top);*/
                this->d2d1devctx->SetTransform(dest_m * canvas_to_preview);

                /*D2D1_ANTIALIAS_MODE old_mode = this->d2d1devctx->GetAntialiasMode();*/
                /*this->d2d1devctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);*/
                this->d2d1devctx->DrawRectangle(dest_rect, 
                    this->box_brush, box_stroke, this->stroke_style);
                /*this->d2d1devctx->SetAntialiasMode(old_mode);*/

                // draw corners
                const int highlighted = highlighted_points;
                ID2D1SolidColorBrush* brush;
                FLOAT corner_length = this->size_point_radius;
                FLOAT corner_stroke = 1.5f;
                this->d2d1devctx->SetTransform(Matrix3x2F::Identity());

                const D2D1_POINT_2F 
                    corner = Point2F(dest_rect.left, dest_rect.top) * dest_m * canvas_to_preview,
                    corner2 = Point2F(dest_rect.right, dest_rect.top) * dest_m * canvas_to_preview,
                    corner3 = Point2F(dest_rect.left, dest_rect.bottom) * dest_m * canvas_to_preview,
                    corner4 = Point2F(dest_rect.right, dest_rect.bottom) * dest_m * canvas_to_preview;

                if((highlighted & control_video2::SCALE_LEFT) &&
                    (highlighted & control_video2::SCALE_TOP))
                    brush = this->highlighted_brush, corner_stroke = 3.f;
                else
                    brush = this->line_brush, corner_stroke = 1.5f;
                this->d2d1devctx->DrawEllipse(Ellipse(corner, corner_length, corner_length), 
                    brush, corner_stroke);
                if((highlighted & control_video2::SCALE_RIGHT) &&
                    (highlighted & control_video2::SCALE_TOP))
                    brush = this->highlighted_brush, corner_stroke = 3.f;
                else
                    brush = this->line_brush, corner_stroke = 1.5f;
                this->d2d1devctx->DrawEllipse(Ellipse(corner2, corner_length, corner_length),
                    brush, corner_stroke);
                if((highlighted & control_video2::SCALE_LEFT) &&
                    (highlighted & control_video2::SCALE_BOTTOM))
                    brush = this->highlighted_brush, corner_stroke = 3.f;
                else
                    brush = this->line_brush, corner_stroke = 1.5f;
                this->d2d1devctx->DrawEllipse(Ellipse(corner3, corner_length, corner_length),
                    brush, corner_stroke);
                if((highlighted & control_video2::SCALE_RIGHT) &&
                    (highlighted & control_video2::SCALE_BOTTOM))
                    brush = this->highlighted_brush, corner_stroke = 3.f;
                else
                    brush = this->line_brush, corner_stroke = 1.5f;
                this->d2d1devctx->DrawEllipse(Ellipse(corner4, corner_length, corner_length),
                    brush, corner_stroke);

                // draw lines
                dest_m = Matrix3x2F::Scale(dest_rect.right - dest_rect.left,
                    dest_rect.bottom - dest_rect.top) * dest_m;
                const FLOAT dest_rotation = video_params.rotate / 180.f * (FLOAT)M_PI;
                const FLOAT x_scale = cos(dest_rotation), y_scale = sin(dest_rotation);
                corner_stroke = 2.f;
                D2D1_POINT_2F center = Point2F(0.5f, 0.f) * dest_m * canvas_to_preview,
                    center2 = Point2F(0.f, 0.5f) * dest_m * canvas_to_preview,
                    center3 = Point2F(0.5f, 1.f) * dest_m * canvas_to_preview,
                    center4 = Point2F(1.f, 0.5f) * dest_m * canvas_to_preview;

                if(highlighted == control_video2::SCALE_TOP)
                    brush = this->highlighted_brush, corner_stroke = 3.5f;
                else
                    brush = this->line_brush, corner_stroke = 2.f;
                this->d2d1devctx->DrawLine(
                    Point2F(center.x - corner_length * y_scale, center.y - corner_length * -x_scale),
                    Point2F(center.x + corner_length * y_scale, center.y + corner_length * -x_scale),
                    brush, corner_stroke);
                if(highlighted == control_video2::SCALE_LEFT)
                    brush = this->highlighted_brush, corner_stroke = 3.5f;
                else
                    brush = this->line_brush, corner_stroke = 2.f;
                this->d2d1devctx->DrawLine(
                    Point2F(center2.x - corner_length * x_scale, center2.y - corner_length * y_scale),
                    Point2F(center2.x + corner_length * x_scale, center2.y + corner_length * y_scale),
                    brush, corner_stroke);
                if(highlighted == control_video2::SCALE_BOTTOM)
                    brush = this->highlighted_brush, corner_stroke = 3.5f;
                else
                    brush = this->line_brush, corner_stroke = 2.f;
                this->d2d1devctx->DrawLine(
                    Point2F(center3.x - corner_length * y_scale, center3.y - corner_length * -x_scale),
                    Point2F(center3.x + corner_length * y_scale, center3.y + corner_length * -x_scale),
                    brush, corner_stroke);
                if(highlighted == control_video2::SCALE_RIGHT)
                    brush = this->highlighted_brush, corner_stroke = 3.5f;
                else
                    brush = this->line_brush, corner_stroke = 2.f;
                this->d2d1devctx->DrawLine(
                    Point2F(center4.x - corner_length * x_scale, center4.y - corner_length * y_scale),
                    Point2F(center4.x + corner_length * x_scale, center4.y + corner_length * y_scale),
                    brush, corner_stroke);
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

D2D1_RECT_F sink_preview2::get_preview_rect() const
{
    scoped_lock lock(this->size_mutex);

    const FLOAT canvas_w = (FLOAT)transform_videomixer::canvas_width;
    const FLOAT canvas_h = (FLOAT)transform_videomixer::canvas_height;
    const FLOAT preview_w = (FLOAT)(this->width - this->padding_width * 2);
    const FLOAT preview_h = (FLOAT)(this->height - this->padding_height * 2);

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

    return D2D1::RectF(
        preview_x, preview_y, 
        preview_x + canvas_w * canvas_scale,
        preview_y + canvas_h * canvas_scale);
}

void sink_preview2::get_window_size(UINT& width, UINT& height) const
{
    scoped_lock lock(this->size_mutex);
    width = this->width;
    height = this->height;
}

void sink_preview2::update_size()
{
    std::lock(*this->context_mutex, this->d2d1_context_mutex, this->size_mutex);
    scoped_lock lock(*this->context_mutex, std::adopt_lock);
    scoped_lock lock2(this->d2d1_context_mutex, std::adopt_lock);
    scoped_lock lock3(this->size_mutex, std::adopt_lock);

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
    return this->sink->session->request_sample(this, rp) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_preview2::process_sample(
    const media_sample& args, request_packet& rp, const media_stream*)
{
    this->sink->draw_sample(
        reinterpret_cast<const media_component_video_args_t&>(args), rp);
    return this->sink->session->give_sample(this, args, rp) ? OK : FATAL_ERROR;
}
