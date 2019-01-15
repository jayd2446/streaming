#include "transform_videomixer.h"
#include "assert.h"
#include <iostream>
#include <algorithm>
#include <limits>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

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
    HRESULT hr = S_OK;

    // TODO: the d2d1 context should probably be in the transform after all
    // create the d2d1 context
    CHECK_HR(hr = this->transform->d2d1dev->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &this->d2d1devctx));

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void stream_videomixer::initialize_buffer(const media_buffer_texture_t& buffer)
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

media_buffer_texture_t stream_videomixer::acquire_buffer(
    const std::shared_ptr<transform_videomixer::buffer_pool>& pool)
{
    transform_videomixer::buffer_pool::scoped_lock lock(pool->mutex);
    if(pool->is_empty())
    {
        media_buffer_texture_t buffer = pool->acquire_buffer();
        this->initialize_buffer(buffer);
        /*std::cout << "creating new..." << std::endl;*/
        return buffer;
    }
    else
    {
        media_buffer_texture_t buffer = pool->acquire_buffer();
        /*std::cout << "reusing..." << std::endl;*/
        return buffer;
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
    media_buffer_texture_t output_buffer = this->acquire_buffer(this->transform->texture_pool);
    const time_unit timestamp = convert_to_time_unit(first,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);
    
    // draw
    this->d2d1devctx->SetTarget(output_buffer->bitmap);

    this->d2d1devctx->BeginDraw();
    this->d2d1devctx->Clear(D2D1::ColorF(D2D1::ColorF::Black));

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
    const HRESULT hr2 = this->d2d1devctx->EndDraw();

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
    out_arg->single_buffer = output_buffer;
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