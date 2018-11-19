#include "transform_videoprocessor2.h"
#include "assert.h"
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

transform_videoprocessor2::transform_videoprocessor2(
    const media_session_t& session, context_mutex_t context_mutex) :
    media_source(session), context_mutex(context_mutex)
{
}

void transform_videoprocessor2::initialize(
    const control_class_t& ctrl_pipeline,
    const CComPtr<ID2D1Factory1>& d2d1factory,
    const CComPtr<ID2D1Device>& d2d1dev,
    const CComPtr<ID3D11Device>& d3d11dev,
    const CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr = S_OK;

    this->ctrl_pipeline = ctrl_pipeline;
    this->d3d11dev = d3d11dev;
    this->d3d11devctx = devctx;
    this->d2d1factory = d2d1factory;
    this->d2d1dev = d2d1dev;

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

stream_videoprocessor2_t transform_videoprocessor2::create_stream()
{
    return stream_videoprocessor2_t(
        new stream_videoprocessor2(this->shared_from_this<transform_videoprocessor2>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_videoprocessor2::stream_videoprocessor2(const transform_videoprocessor2_t& transform) :
    transform(transform),
    output_buffer(new media_buffer_texture),
    samples_received(0)
{
    HRESULT hr = S_OK;

    // create the d2d1 context
    CHECK_HR(hr = this->transform->d2d1dev->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &this->d2d1devctx));

    // create the d3d11 texture
    {
        D3D11_TEXTURE2D_DESC desc;
        desc.Width = transform_h264_encoder::frame_width;
        desc.Height = transform_h264_encoder::frame_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        CHECK_HR(hr = this->transform->d3d11dev->CreateTexture2D(
            &desc, NULL, &this->output_buffer->texture));
    }

    // associate the texture with d2d1 bitmap which is used as a render target
    // for the d2d1 dev ctx
    {
        CComPtr<IDXGISurface> dxgisurface;
        D2D1_BITMAP_PROPERTIES1 bitmap_properties =
            D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

        CHECK_HR(hr = this->output_buffer->texture->QueryInterface(&dxgisurface));
        CHECK_HR(hr = this->d2d1devctx->CreateBitmapFromDxgiSurface(
            dxgisurface, bitmap_properties, &this->output_buffer->bitmap));

        this->d2d1devctx->SetTarget(this->output_buffer->bitmap);
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void stream_videoprocessor2::calculate_stream_rects(
    const media_sample_videoprocessor2::params_t& stream_params,
    const media_sample_videoprocessor2::params_t& user_params,
    D2D1_RECT_F& src_rect, D2D1_RECT_F& dst_rect)
{
    // user src rect and sample dst rect are used to calculate the src rect
    // (the intersect is taken to calculate non overlapping areas)
    // dst rect size is modified if there are non overlapping areas
    // and src rect size aswell

    // if user src rect is larger, dst rect size is only modified
    // if sample dst rect is larger, src rect size is oly modified

    RECT user_src_rect, user_dst_rect;
    user_src_rect = user_params.source_rect;
    user_dst_rect = user_params.dest_rect;

    RECT sample_src_rect, sample_dst_rect;
    sample_src_rect = stream_params.source_rect;
    sample_dst_rect = stream_params.dest_rect;

    double sample_src_width, sample_src_height;
    sample_src_width = sample_src_rect.right - sample_src_rect.left;
    sample_src_height = sample_src_rect.bottom - sample_src_rect.top;

    double sample_dst_width, sample_dst_height;
    sample_dst_width = sample_dst_rect.right - sample_dst_rect.left;
    sample_dst_height = sample_dst_rect.bottom - sample_dst_rect.top;

    double user_src_width, user_src_height;
    user_src_width = user_src_rect.right - user_src_rect.left;
    user_src_height = user_src_rect.bottom - user_src_rect.top;

    double user_dst_width, user_dst_height;
    user_dst_width = user_dst_rect.right - user_dst_rect.left;
    user_dst_height = user_dst_rect.bottom - user_dst_rect.top;

    RECT nonoverlapping_rect;
    nonoverlapping_rect.top = user_src_rect.top - sample_dst_rect.top;
    nonoverlapping_rect.left = user_src_rect.left - sample_dst_rect.left;
    // these are negative
    nonoverlapping_rect.bottom = user_src_rect.bottom - sample_dst_rect.bottom;
    nonoverlapping_rect.right = user_src_rect.right - sample_dst_rect.right;

    src_rect = D2D1::RectF(
        (FLOAT)sample_src_rect.left, (FLOAT)sample_src_rect.top,
        (FLOAT)sample_src_rect.right, (FLOAT)sample_src_rect.bottom);
    dst_rect = D2D1::RectF(
        (FLOAT)user_dst_rect.left, (FLOAT)user_dst_rect.top,
        (FLOAT)user_dst_rect.right, (FLOAT)user_dst_rect.bottom);

    if(nonoverlapping_rect.top > 0)
        src_rect.top += (FLOAT)(sample_src_height * (nonoverlapping_rect.top / sample_dst_height));
    else if(nonoverlapping_rect.top < 0)
        dst_rect.top -= (FLOAT)(user_dst_height * (nonoverlapping_rect.top / user_src_height));
    if(nonoverlapping_rect.left > 0)
        src_rect.left += (FLOAT)(sample_src_width * (nonoverlapping_rect.left / sample_dst_width));
    else if(nonoverlapping_rect.left < 0)
        dst_rect.left -= (FLOAT)(user_dst_width * (nonoverlapping_rect.left / user_src_width));
    if(nonoverlapping_rect.bottom < 0)
        src_rect.bottom += (FLOAT)(sample_src_height * (nonoverlapping_rect.bottom / sample_dst_height));
    else if(nonoverlapping_rect.bottom > 0)
        dst_rect.bottom -= (FLOAT)(user_dst_height * (nonoverlapping_rect.bottom / user_src_height));
    if(nonoverlapping_rect.right < 0)
        src_rect.right += (FLOAT)(sample_src_width * (nonoverlapping_rect.right / sample_dst_width));
    else if(nonoverlapping_rect.right > 0)
        dst_rect.right -= (FLOAT)(user_dst_width * (nonoverlapping_rect.right / user_src_width));
}

void stream_videoprocessor2::process()
{
    HRESULT hr = S_OK;
    media_sample_texture sample;
    time_unit timestamp = std::numeric_limits<time_unit>::max();
    // rps are equivalent in every input stream
    request_packet rp = this->input_streams[0].first.rp;

    D2D1::Matrix3x2F matrix = D2D1::Matrix3x2F::Identity();
    this->d2d1devctx->BeginDraw();
    this->d2d1devctx->Clear(D2D1::ColorF(D2D1::ColorF::Black));

    for(auto&& stream : this->input_streams)
    {
        CComPtr<ID3D11Texture2D> texture = stream.first.sample.buffer->texture;
        const media_sample_videoprocessor2::params_t& params =
            stream.first.sample.params;
        media_sample_videoprocessor2::params_t user_params;

        if(stream.first.user_params)
            stream.first.user_params->get_params(user_params);
        else
            user_params = params;

        // use the earliest timestamp;
        // actually, max must be used so that the timestamp stays incremental
        // (using max only applies to displaycapture where the buffers are shared between streams)
        timestamp = rp.request_time;//std::min(timestamp, it->first.sample_view.timestamp);

        if(!texture)
            continue;

        // TODO: include bitmap in texture sample
        // create bitmap
        CComPtr<ID2D1Bitmap1> bitmap;
        CComPtr<IDXGISurface> surface;
        CHECK_HR(hr = texture->QueryInterface(&surface));
        CHECK_HR(hr = this->d2d1devctx->CreateBitmapFromDxgiSurface(
            surface,
            D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &bitmap));

        D2D1_RECT_F src_rect, dst_rect;
        this->calculate_stream_rects(params, user_params, src_rect, dst_rect);

        this->d2d1devctx->SetTransform(D2D1::Matrix3x2F::Identity());
        this->d2d1devctx->DrawBitmap(
            bitmap,
            dst_rect,
            1.f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
            src_rect);
    }

done:
    CHECK_HR(hr = this->d2d1devctx->EndDraw());

    if(FAILED(hr))
    {
        if(hr != D2DERR_RECREATE_TARGET)
            throw HR_EXCEPTION(hr);

        PRINT_ERROR(hr);
        this->transform->request_reinitialization(this->transform->ctrl_pipeline);
    }

    sample.buffer = this->output_buffer;
    sample.timestamp = timestamp;

    // reset the sample view from the input stream packets so it is unlocked;
    // reset the rps so that there aren't any circular dependencies at shutdown
    for(auto&& stream : this->input_streams)
    {
        stream.first.sample.buffer = NULL;
        stream.first.sample.timestamp = -1;
        stream.first.rp = request_packet();
    }

    this->transform->session->give_sample(this, sample, rp, false);
}

void stream_videoprocessor2::connect_streams(
    const media_stream_t& from,
    const stream_videoprocessor2_controller_t& user_params,
    const media_topology_t& topology)
{
    this->input_streams.push_back(std::make_pair(packet(), from.get()));
    this->input_streams.back().first.user_params = user_params;

    media_stream::connect_streams(from, topology);
}

media_stream::result_t stream_videoprocessor2::request_sample(
    request_packet& rp, const media_stream*)
{
    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_videoprocessor2::process_sample(
    const media_sample& sample_, request_packet& rp, const media_stream* prev_stream)
{
    const media_sample_videoprocessor2& sample =
        reinterpret_cast<const media_sample_videoprocessor2&>(sample_);

    // this function needs to be locked because multiple inputs
    // might be running simultaneously
    std::unique_lock<std::recursive_mutex> lock(this->mutex);

    {
        // TODO: sample view could include an index so that the input stream
        // doesn't need to be searched for
        auto it = std::find_if(this->input_streams.begin(), this->input_streams.end(),
            [prev_stream](const input_stream_t& e) {
            return e.second == prev_stream && e.first.sample.timestamp == -1;});
        assert_(it != this->input_streams.end());

        it->first.rp = rp;
        it->first.sample = sample;
        this->samples_received++;

        assert_(this->samples_received <= (int)this->input_streams.size());
        if(this->samples_received == (int)this->input_streams.size())
        {
            this->samples_received = 0;
            this->process();
        }
    }

    return OK;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


void stream_videoprocessor2_controller::get_params(params_t& params) const
{
    scoped_lock lock(this->mutex);
    params = this->params;
}

void stream_videoprocessor2_controller::set_params(const params_t& params)
{
    scoped_lock lock(this->mutex);
    this->params = params;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_videoprocessor2::media_sample_videoprocessor2(
    const media_buffer_texture_t& texture_buffer) :
    media_sample_texture(texture_buffer)
{
}

media_sample_videoprocessor2::media_sample_videoprocessor2(
    const params_t& params, const media_buffer_texture_t& texture_buffer) :
    media_sample_texture(texture_buffer),
    params(params)
{
}
