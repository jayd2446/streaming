#include "transform_videoprocessor2.h"
#include "assert.h"
#include <iostream>
#include <limits>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

transform_videoprocessor2::transform_videoprocessor2(
    const media_session_t& session, context_mutex_t context_mutex) :
    media_source(session), context_mutex(context_mutex), texture_pool(new buffer_pool)
{
}

transform_videoprocessor2::~transform_videoprocessor2()
{
    buffer_pool::scoped_lock lock(this->texture_pool->mutex);
    this->texture_pool->dispose();
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
    transform(transform)
{
    HRESULT hr = S_OK;

    // create the d2d1 context
    CHECK_HR(hr = this->transform->d2d1dev->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &this->d2d1devctx));

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void stream_videoprocessor2::initialize_buffer(const media_buffer_texture_t& buffer)
{
    HRESULT hr = S_OK;
    CComPtr<IDXGISurface> dxgisurface;
    D2D1_BITMAP_PROPERTIES1 bitmap_properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

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
    CHECK_HR(hr = this->transform->d3d11dev->CreateTexture2D(&desc, NULL, &buffer->texture));

    CHECK_HR(hr = buffer->texture->QueryInterface(&dxgisurface));
    CHECK_HR(hr = this->d2d1devctx->CreateBitmapFromDxgiSurface(
        dxgisurface, bitmap_properties, &buffer->bitmap));

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void stream_videoprocessor2::process(request_queue::request_t& request)
{
    // the stream in the request equals to this

    HRESULT hr = S_OK;
    media_sample_texture sample;
    const time_unit timestamp = request.rp.request_time;

    // get the pooled buffer and set the device context to draw into it
    media_buffer_texture_t output_buffer;
    {
        transform_videoprocessor2::buffer_pool::scoped_lock lock(this->transform->texture_pool->mutex);
        if(this->transform->texture_pool->container.empty())
        {
            media_buffer_pooled_texture_t pooled_buffer(new media_buffer_pooled_texture(
                this->transform->texture_pool));
            output_buffer = pooled_buffer->create_pooled_buffer();
            this->initialize_buffer(output_buffer);
        }
        else
        {
            output_buffer = this->transform->texture_pool->container.top()->create_pooled_buffer();
            this->transform->texture_pool->container.pop();
        }
    }
    this->d2d1devctx->SetTarget(output_buffer->bitmap);

    // draw
    this->d2d1devctx->BeginDraw();
    this->d2d1devctx->Clear(D2D1::ColorF(D2D1::ColorF::Black));

    for(size_t i = 0; i < this->input_stream_props.size(); i++)
    {
        CComPtr<ID3D11Texture2D> texture = request.sample_view.second[i].sample.buffer->texture;
        const media_sample_videoprocessor2::params_t& params = 
            request.sample_view.second[i].sample.params;
        const media_sample_videoprocessor2::params_t& user_params = 
            request.sample_view.second[i].valid_user_params ? 
            request.sample_view.second[i].user_params : params;

        // use the earliest timestamp;
        // actually, max must be used so that the timestamp stays incremental
        // (using max only applies to displaycapture where the buffers are shared between streams)
        //timestamp = rp.request_time;//std::min(timestamp, it->first.sample_view.timestamp);

        if(!texture)
            continue;

        // TODO: include bitmap in texture sample
        // create bitmap

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

        using namespace D2D1;
        CComPtr<ID2D1Bitmap1> bitmap;
        CComPtr<IDXGISurface> surface;
        CComPtr<ID2D1BitmapBrush1> bitmap_brush;
        Matrix3x2F src_to_dest, src2_to_dest2;
        Matrix3x2F world, brush;
        bool invert;

        // src_rect_m * M = dest_rect_m <=> M = src_rect_t -1 * dest_rect_m
        src_to_dest = Matrix3x2F::Scale(
            params.source_rect.right - params.source_rect.left,
            params.source_rect.bottom - params.source_rect.top) *
            Matrix3x2F::Translation(params.source_rect.left, params.source_rect.top);
        invert = src_to_dest.Invert();
        src_to_dest = src_to_dest * Matrix3x2F::Scale(
            params.dest_rect.right - params.dest_rect.left,
            params.dest_rect.bottom - params.dest_rect.top) *
            Matrix3x2F::Translation(params.dest_rect.left, params.dest_rect.top) *
            params.dest_m;

        src2_to_dest2 = Matrix3x2F::Scale(
            user_params.source_rect.right - user_params.source_rect.left,
            user_params.source_rect.bottom - user_params.source_rect.top) *
            Matrix3x2F::Translation(user_params.source_rect.left, user_params.source_rect.top) *
            user_params.source_m;
        invert = src2_to_dest2.Invert();
        src2_to_dest2 = src2_to_dest2 * Matrix3x2F::Scale(
            user_params.dest_rect.right - user_params.dest_rect.left,
            user_params.dest_rect.bottom - user_params.dest_rect.top) *
            Matrix3x2F::Translation(user_params.dest_rect.left, user_params.dest_rect.top) *
            user_params.dest_m;

        world = src_to_dest * src2_to_dest2;
        brush = params.source_m;

        /*
        (multiplication of translation matrices are commutative, probably same applies to
        rotation and scaling),
        affine transformation matrix cannot map a set of arbitrary points to another set,
        it only maps sets that preserve affinity
        https://en.wikipedia.org/wiki/Affine_transformation#Properties_preserved

        linear mappings preserve the origin, which isn't the case in affine mappings
        (translation matrix)

        3x2 matrix(affine transformation matrix) in directx:
        a       b       (0)
        c       d       (0)
        e(dx)   f(dy)   (1), in opengl the matrix is transposed

        in directx, the matrix is premultiplied by the vector point, which
        is P' = P M, whereas in opengl it is post multiplied:
        P' = M P

        this means that the ordering in opengl is different
        (glscale, gltranslate) will result in translating first, scaling second

        opengl also uses column vectors while directx uses row vectors
        (the transformation matrices are thus also different);

        the fact that both the multiplication and vectors differ, the transformation
        matrices can be used in both opengl and directx;
        this probably relates to the fact that (AB) transposed = B transposed * A transposed

        */

        CHECK_HR(hr = texture->QueryInterface(&surface));
        CHECK_HR(hr = this->d2d1devctx->CreateBitmapFromDxgiSurface(
            surface,
            BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &bitmap));
        CHECK_HR(hr = this->d2d1devctx->CreateBitmapBrush(bitmap, &bitmap_brush));

        bitmap_brush->SetTransform(brush);

        if(!user_params.axis_aligned_clip)
        {
            CComPtr<ID2D1RectangleGeometry> geometry;
            D2D1_LAYER_PARAMETERS1 layer_params;
            Matrix3x2F world_inverted, layer;

            world_inverted = world;
            invert = world_inverted.Invert();
            layer = user_params.dest_m * world_inverted;

            CHECK_HR(hr = this->transform->d2d1factory->CreateRectangleGeometry(
                user_params.dest_rect, &geometry));
            layer_params = LayerParameters1(
                InfiniteRect(), geometry, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, layer);

            this->d2d1devctx->SetTransform(world);
            this->d2d1devctx->PushLayer(layer_params, NULL);
        }
        else
        {
            this->d2d1devctx->SetTransform(user_params.dest_m);
            // the world transform is applied to the axis aligned clip when push is called
            this->d2d1devctx->PushAxisAlignedClip(
                user_params.dest_rect, 
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            this->d2d1devctx->SetTransform(world);
        }

        this->d2d1devctx->FillRectangle(params.source_rect, bitmap_brush);

        if(!user_params.axis_aligned_clip)
            this->d2d1devctx->PopLayer();
        else
            this->d2d1devctx->PopAxisAlignedClip();
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

    sample.buffer = output_buffer;
    sample.timestamp = timestamp;

    this->unlock();
    this->transform->session->give_sample(request.stream, sample, request.rp, false);
}

void stream_videoprocessor2::connect_streams(
    const media_stream_t& from,
    const stream_videoprocessor2_controller_t& user_params,
    const media_topology_t& topology)
{
    input_stream_props_t props;
    props.input_stream = from.get();
    props.user_params = user_params;
    this->input_stream_props.insert(this->input_stream_props.begin(), props);

    media_stream::connect_streams(from, topology);
}

media_stream::result_t stream_videoprocessor2::request_sample(
    request_packet& rp, const media_stream*)
{
    this->requests.initialize_queue(rp);

    // build the request partially
    request_t packets;
    packets.first = 0;
    // TODO: array pool could be used
    packets.second.reset(new packet[this->input_stream_props.size()]);
    for(size_t i = 0; i < this->input_stream_props.size(); i++)
    {
        packets.second[i].input_stream = this->input_stream_props[i].input_stream;
        packets.second[i].valid_user_params = !!this->input_stream_props[i].user_params;
        if(packets.second[i].valid_user_params)
            this->input_stream_props[i].user_params->get_params(packets.second[i].user_params);
    }

    // add the new request to the queue
    request_queue::request_t request;
    request.rp = rp;
    request.stream = this;
    request.sample_view = packets;
    this->requests.push(request);

    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_videoprocessor2::process_sample(
    const media_sample& sample_, request_packet& rp, const media_stream* prev_stream)
{
    this->lock();

    const media_sample_videoprocessor2& sample =
        static_cast<const media_sample_videoprocessor2&>(sample_);

    request_queue::request_t* request = this->requests.get(rp.packet_number);
    assert_(request);

    /*Sleep(10);*/

    // find the right packet from the list
    packet* p = NULL;
    for(size_t i = 0; i < this->input_stream_props.size(); i++)
        if(request->sample_view.second[i].input_stream == prev_stream &&
            request->sample_view.second[i].sample.timestamp == -1)
        {
            p = request->sample_view.second.get() + i;
            break;
        };
    assert_(p);

    request->sample_view.first++;
    p->sample = sample;

    // TODO: drop topology branching and store the queue in stream;
    // that way the items in the queue are from the same stream

    // TODO: drop topology branching all together; the current model is a superset of that;
    // buffer pools provide similar functionality to duplicated stream resources that are
    // non locking

    // dispatch all requests that are ready
    for(request_queue::request_t* next_request = this->requests.get();
        next_request && next_request->sample_view.first == this->input_stream_props.size();
        next_request = this->requests.get())
    {
        assert_(next_request->stream == this);

        request_queue::request_t request;
        this->requests.pop(request);

        this->process(request);
        this->lock();
    }

    this->unlock();
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
