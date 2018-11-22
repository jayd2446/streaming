#include "transform_videoprocessor2.h"
#include "assert.h"
#include <iostream>
#include <limits>

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
    this->ctrl_pipeline = ctrl_pipeline;
    this->d3d11dev = d3d11dev;
    this->d3d11devctx = devctx;
    this->d2d1factory = d2d1factory;
    this->d2d1dev = d2d1dev;
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

void stream_videoprocessor2::process()
{
    HRESULT hr = S_OK;
    media_sample_texture sample;
    time_unit timestamp = std::numeric_limits<time_unit>::max();
    // rps are equivalent in every input stream
    request_packet rp = this->input_streams[0].first.rp;

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
        using namespace D2D1;
        CComPtr<ID2D1Bitmap1> bitmap;
        CComPtr<IDXGISurface> surface;
        CComPtr<ID2D1BitmapBrush1> bitmap_brush;
        CComPtr<ID2D1RectangleGeometry> geometry;
        Matrix3x2F src, dst, src2, dst2;
        Matrix3x2F src_to_dst, src2_to_dst2;
        Matrix3x2F world_matrix, inverse_world, layer_matrix;
        D2D1_LAYER_PARAMETERS1 layer_params;
        bool invert;
        static FLOAT angle = 0.f;
        const FLOAT src_angle = 0.f, dst_angle = 0.f;
        const FLOAT src2_angle = 0.f, dst2_angle = 0.f;
        static const D2D1_RECT_F clip_rect = RectF(0.f, 0.f, 1.f, 1.f);
        // used when the axis aligned clip is applicable
        D2D1_RECT_F new_clip_rect;
        bool use_axis_aligned_clip = true;

        /*
        ax = b <=> x = a-1 b,
        xa = b <=> x = b a-1
        the order of multiplication is important, because
        almost never ab = ba

        vector is a matrix with one column

        A 1 × n matrix is called a row vector. 
        Direct2D and Direct3D both use row vectors to represent points in 2D or 3D space.

        Most graphics texts use the column vector form.
        */

        /*
        3x2 matrix:
        x y
        x y
        x y

        vector:
        x
        y
        */

        /*angle += 0.02f;*/

        CHECK_HR(hr = texture->QueryInterface(&surface));
        CHECK_HR(hr = this->d2d1devctx->CreateBitmapFromDxgiSurface(
            surface,
            BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &bitmap));
        CHECK_HR(hr = this->d2d1devctx->CreateBitmapBrush(bitmap, &bitmap_brush));

        src = Matrix3x2F::Scale(params.source_rect.right - params.source_rect.left,
            params.source_rect.bottom - params.source_rect.top) *
            Matrix3x2F::Rotation(src_angle) *
            Matrix3x2F::Translation(params.source_rect.left, params.source_rect.top);
        dst = Matrix3x2F::Scale(params.dest_rect.right - params.dest_rect.left,
            params.dest_rect.bottom - params.dest_rect.top) *
            Matrix3x2F::Rotation(dst_angle) *
            Matrix3x2F::Translation(params.dest_rect.left, params.dest_rect.top);
        src2 = Matrix3x2F::Scale(user_params.source_rect.right - user_params.source_rect.left,
            user_params.source_rect.bottom - user_params.source_rect.top) *
            Matrix3x2F::Rotation(src2_angle) *
            Matrix3x2F::Translation(user_params.source_rect.left, user_params.source_rect.top);
        dst2 = Matrix3x2F::Scale(user_params.dest_rect.right - user_params.dest_rect.left,
            user_params.dest_rect.bottom - user_params.dest_rect.top) *
            Matrix3x2F::Rotation(dst2_angle) *
            Matrix3x2F::Translation(user_params.dest_rect.left, user_params.dest_rect.top);

        src_to_dst = src;
        invert = src_to_dst.Invert();
        src_to_dst = src_to_dst * dst;
        src2_to_dst2 = src2;
        invert = src2_to_dst2.Invert();
        src2_to_dst2 = src2_to_dst2 * dst2;

        world_matrix = src_to_dst * src2_to_dst2;
        inverse_world = world_matrix;
        invert = inverse_world.Invert();

        layer_matrix = dst2 * inverse_world;

        this->d2d1devctx->SetTransform(world_matrix);

        if(std::abs(src_angle) < std::numeric_limits<FLOAT>::epsilon() &&
            std::abs(dst_angle) < std::numeric_limits<FLOAT>::epsilon() &&
            std::abs(src2_angle) < std::numeric_limits<FLOAT>::epsilon() &&
            std::abs(dst2_angle) < std::numeric_limits<FLOAT>::epsilon() && use_axis_aligned_clip)
        {
            new_clip_rect = RectF(
                layer_matrix.m[0][0] * clip_rect.left + layer_matrix.m[1][0] * clip_rect.top +
                layer_matrix.m[2][0],
                layer_matrix.m[0][1] * clip_rect.left + layer_matrix.m[1][1] * clip_rect.top +
                layer_matrix.m[2][1],

                layer_matrix.m[0][0] * clip_rect.right + layer_matrix.m[1][0] * clip_rect.bottom +
                layer_matrix.m[2][0],
                layer_matrix.m[0][1] * clip_rect.right + layer_matrix.m[1][1] * clip_rect.bottom +
                layer_matrix.m[2][1]);
        }
        else
        {
            use_axis_aligned_clip = false;

            // TODO: rectangle geometry shouldn't be reallocated every time
            CHECK_HR(hr = this->transform->d2d1factory->CreateRectangleGeometry(
                clip_rect, &geometry));
            layer_params = LayerParameters1(
                InfiniteRect(), geometry, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                layer_matrix);
        }

        if(use_axis_aligned_clip)
            this->d2d1devctx->PushAxisAlignedClip(new_clip_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        else
            this->d2d1devctx->PushLayer(layer_params, NULL);

        this->d2d1devctx->FillRectangle(RectF(
            params.source_rect.left, params.source_rect.top,
            params.source_rect.right, params.source_rect.bottom), bitmap_brush);

        if(use_axis_aligned_clip)
            this->d2d1devctx->PopAxisAlignedClip();
        else
            this->d2d1devctx->PopLayer();
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
