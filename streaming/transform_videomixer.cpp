#include "transform_videomixer.h"
#include "assert.h"
#include <iostream>
#include <algorithm>
#include <limits>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

struct transform_videomixer::device_context_resources : media_buffer_texture
{
    CComPtr<ID2D1DeviceContext> ctx;
    // the texture that is bound to this brush must be immutable;
    // it is assumed that the input samples are immutable
    CComPtr<ID2D1BitmapBrush1> bitmap_brush;

    virtual ~device_context_resources() {}
    void uninitialize() {}
};

transform_videomixer::transform_videomixer(
    const media_session_t& session, context_mutex_t context_mutex) :
    transform_videomixer_base(session), context_mutex(context_mutex), texture_pool(new buffer_pool)
{
}

transform_videomixer::~transform_videomixer()
{
    buffer_pool::scoped_lock lock(this->texture_pool->mutex);
    this->texture_pool->dispose();
}

void transform_videomixer::initialize(
    const control_class_t& ctrl_pipeline,
    const CComPtr<ID2D1Factory1>& d2d1factory,
    const CComPtr<ID2D1Device>& d2d1dev,
    const CComPtr<ID3D11Device>& d3d11dev,
    const CComPtr<ID3D11DeviceContext>& devctx)
{
    transform_videomixer_base::initialize(transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);

    this->ctrl_pipeline = ctrl_pipeline;
    this->d3d11dev = d3d11dev;
    this->d3d11devctx = devctx;
    this->d2d1factory = d2d1factory;
    this->d2d1dev = d2d1dev;
}

transform_videomixer::stream_mixer_t transform_videomixer::create_derived_stream()
{
    return stream_videomixer_base_t(
        new stream_videomixer(this->shared_from_this<transform_videomixer>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_videomixer::stream_videomixer(const transform_videomixer_t& transform) :
    stream_mixer(transform),
    transform(transform)
{
}

void stream_videomixer::initialize_resources(const device_context_resources_t& resources)
{
    HRESULT hr = S_OK;

    CComPtr<IDXGISurface> dxgisurface;
    D2D1_BITMAP_PROPERTIES1 output_bitmap_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    // initialize the media_buffer_texture part
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
    resources->initialize(this->transform->d3d11dev, desc, NULL);

    // initialize resources
    CHECK_HR(hr = resources->texture->QueryInterface(&dxgisurface));
    CHECK_HR(hr = this->transform->d2d1dev->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &resources->ctx));
    CHECK_HR(hr = resources->ctx->CreateBitmapFromDxgiSurface(
        dxgisurface, output_bitmap_props, &resources->bitmap));
    // set the initial source for bitmap brush to output texture;
    // it will be switched when mixing
    CHECK_HR(hr = resources->ctx->CreateBitmapBrush(resources->bitmap, &resources->bitmap_brush));

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

stream_videomixer::device_context_resources_t stream_videomixer::acquire_buffer()
{
    transform_videomixer::buffer_pool::scoped_lock lock(this->transform->texture_pool->mutex);
    if(this->transform->texture_pool->is_empty())
    {
        device_context_resources_t resources = this->transform->texture_pool->acquire_buffer();
        this->initialize_resources(resources);
        return resources;
    }
    else
    {
        device_context_resources_t resources = this->transform->texture_pool->acquire_buffer();
        return resources;
    }
}

bool stream_videomixer::move_frames(in_arg_t& in_arg, in_arg_t& old_in_arg, frame_unit end,
    bool discarded)
{
    assert_(old_in_arg);
    // TODO: allow multiple buffer

    in_arg = std::make_optional<in_arg_t::value_type>();

    // the parameters of the old sample must be updated when moving frames;
    // aswell as the parameters for the new sample must be set

    if(old_in_arg->frame_end <= end)
    {
        in_arg = old_in_arg;
        old_in_arg.reset();

        /*sample.single_buffer = std::move(old_sample.single_buffer);*/

        // whole sample was moved, which means that its parameters doesn't need to be updated
        // since it is discarded
        return true;
    }

    return false;
}

void stream_videomixer::mix(out_arg_t& out_arg, args_t& packets,
    frame_unit first, frame_unit end)
{
    // the samples in packets might be null

    assert_(!packets.container.empty());

    // the streams in the packet are sorted

    // TODO: use media_buffer_textures

    HRESULT hr = S_OK;
    device_context_resources_t context = this->acquire_buffer();
    const time_unit timestamp = convert_to_time_unit(first,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);
    
    // draw
    context->ctx->SetTarget(context->bitmap);

    context->ctx->BeginDraw();
    context->ctx->Clear(D2D1::ColorF(D2D1::ColorF::Black));

    // TODO: videomixer shouldn't output silent frames so that
    // the encoder doesn't need to handle it

    // TODO: for non variable fps, videomixer should allocate frames from first to end
    for(auto&& item : packets.container)
    {
        if(!item.arg || !item.arg->single_buffer || !item.arg->single_buffer->texture)
            continue;

        CComPtr<ID3D11Texture2D> texture = item.arg->single_buffer->texture;
        const stream_videomixer_controller::params_t& params = item.arg->params;
        const stream_videomixer_controller::params_t& user_params =
            item.valid_user_params ? item.user_params : params;

        /////////////////////////////////////////////////////////////////
        /////////////////////////////////////////////////////////////////
        /////////////////////////////////////////////////////////////////

        // TODO: cache d2d1 resources instead of reallocating

        using namespace D2D1;
        CComPtr<ID2D1Bitmap1> bitmap;
        CComPtr<IDXGISurface> surface;
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

        CHECK_HR(hr = texture->QueryInterface(&surface));
        CHECK_HR(hr = context->ctx->CreateBitmapFromDxgiSurface(
            surface,
            BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &bitmap));
        context->bitmap_brush->SetBitmap(bitmap);

        context->bitmap_brush->SetTransform(brush);

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

            context->ctx->SetTransform(world);
            context->ctx->PushLayer(layer_params, NULL);
        }
        else
        {
            context->ctx->SetTransform(user_params.dest_m);
            // the world transform is applied to the axis aligned clip when push is called
            context->ctx->PushAxisAlignedClip(
                user_params.dest_rect,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            context->ctx->SetTransform(world);
        }

        context->ctx->FillRectangle(params.source_rect, context->bitmap_brush);

        if(!user_params.axis_aligned_clip)
            context->ctx->PopLayer();
        else
            context->ctx->PopAxisAlignedClip();
    }

done:
    const HRESULT hr2 = context->ctx->EndDraw();

    if(FAILED(hr) || FAILED(hr2))
    {
        if(hr != D2DERR_RECREATE_TARGET)
            throw HR_EXCEPTION(hr);
        if(hr2 != D2DERR_RECREATE_TARGET)
            throw HR_EXCEPTION(hr2);

        if(FAILED(hr))
            PRINT_ERROR(hr);
        if(FAILED(hr2))
            PRINT_ERROR(hr2);

        this->transform->request_reinitialization(this->transform->ctrl_pipeline);
    }

    out_arg = std::make_optional<out_arg_t::value_type>();
    out_arg->frame_end = end;
    out_arg->single_buffer = context;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


void stream_videomixer_controller::get_params(params_t& params) const
{
    scoped_lock lock(this->mutex);
    params = this->params;
}

void stream_videomixer_controller::set_params(const params_t& params)
{
    scoped_lock lock(this->mutex);
    this->params = params;
}