#include "source_displaycapture.h"
#include "control_pipeline.h"
#include "transform_h264_encoder.h"
#include <iostream>
#include <d2d1.h>
#include <Mferror.h>

#pragma comment(lib, "D3D11.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
#ifdef _DEBUG
#define CREATE_DEVICE_DEBUG D3D11_CREATE_DEVICE_DEBUG
#else
#define CREATE_DEVICE_DEBUG 0
#endif
#undef max
#undef min

class displaycapture_exception : public std::exception {};


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


source_displaycapture::source_displaycapture(
    const media_session_t& session, context_mutex_t context_mutex) :
    source_base(session),
    context_mutex(context_mutex),
    output_index((UINT)-1),
    same_adapter(false),
    available_samples(new buffer_pool),
    available_pointer_samples(new buffer_pool)
{
    this->outdupl_desc.Rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    this->pointer_position.Visible = FALSE;
    this->pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
}

source_displaycapture::~source_displaycapture()
{
    {
        buffer_pool::scoped_lock lock(this->available_samples->mutex);
        this->available_samples->dispose();
    }
    {
        buffer_pool::scoped_lock lock(this->available_pointer_samples->mutex);
        this->available_pointer_samples->dispose();
    }
}

source_displaycapture::stream_source_base_t source_displaycapture::create_derived_stream()
{
    return stream_displaycapture_t(new stream_displaycapture(
        this->shared_from_this<source_displaycapture>()));
}

media_stream_t source_displaycapture::create_pointer_stream(
    const stream_displaycapture_t& displaycapture_stream)
{
    stream_displaycapture_pointer_t pointer_stream(new stream_displaycapture_pointer(
        this->shared_from_this<source_displaycapture>()));
    displaycapture_stream->pointer_stream = pointer_stream;

    return pointer_stream;
}

bool source_displaycapture::get_samples_end(time_unit request_time, frame_unit& end)
{
    // note: time shifting isn't possible here, unless the drain is properly handled aswell

    // displaycapture just pretends that it has samples up to the request point
    end = convert_to_frame_unit(request_time,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);

    return true;
}

void source_displaycapture::make_request(request_t& request, frame_unit frame_end)
{
    // capture the frame and populate the request

    auto build_frame = [this, frame_end](media_sample_video_mixer_frame& frame)
    {
        frame.pos = frame_end - 1;
        frame.dur = 1;

        if(!frame.buffer)
            return;

        D3D11_TEXTURE2D_DESC desc;
        frame.buffer->texture->GetDesc(&desc);

        frame.params.source_rect.top = frame.params.source_rect.left = 0.f;
        frame.params.source_rect.right = (FLOAT)desc.Width;
        frame.params.source_rect.bottom = (FLOAT)desc.Height;
        frame.params.dest_rect = frame.params.source_rect;
        frame.params.source_m = frame.params.dest_m = D2D1::Matrix3x2F::Identity();

        using namespace D2D1;
        switch(this->outdupl_desc.Rotation)
        {
        case DXGI_MODE_ROTATION_ROTATE90:
            frame.params.dest_m = Matrix3x2F::Rotation(90.f) *
                Matrix3x2F::Translation((FLOAT)desc.Height, 0.f);
            break;
        case DXGI_MODE_ROTATION_ROTATE180:
            frame.params.dest_m = Matrix3x2F::Rotation(180.f) *
                Matrix3x2F::Translation((FLOAT)desc.Width, (FLOAT)desc.Height);
            break;
        case DXGI_MODE_ROTATION_ROTATE270:
            frame.params.dest_m = Matrix3x2F::Rotation(270.f) *
                Matrix3x2F::Translation(0.f, (FLOAT)desc.Width);
            break;
        }
    };
    auto build_pointer_frame = [this, frame_end](media_sample_video_mixer_frame& pointer_frame)
    {
        pointer_frame.pos = frame_end - 1;
        pointer_frame.dur = 1;

        if(!pointer_frame.buffer || !this->pointer_position.Visible)
        {
            pointer_frame.buffer = NULL;
            return;
        }

        D3D11_TEXTURE2D_DESC desc;
        pointer_frame.buffer->texture->GetDesc(&desc);

        pointer_frame.params.source_rect.left = pointer_frame.params.source_rect.top = 0.f;
        pointer_frame.params.source_rect.right = (FLOAT)desc.Width;
        pointer_frame.params.source_rect.bottom = (FLOAT)desc.Height;

        pointer_frame.params.dest_rect.left = (FLOAT)pointer_position.Position.x;
        pointer_frame.params.dest_rect.top = (FLOAT)pointer_position.Position.y;
        pointer_frame.params.dest_rect.right = pointer_frame.params.dest_rect.left + (FLOAT)desc.Width;
        pointer_frame.params.dest_rect.bottom = pointer_frame.params.dest_rect.top + (FLOAT)desc.Height;

        pointer_frame.params.source_m = D2D1::Matrix3x2F::Identity();
        pointer_frame.params.dest_m = D2D1::Matrix3x2F::Identity();
    };

    request.sample.args = std::make_optional<displaycapture_args>();
    media_component_videomixer_args& args = request.sample.args->args;
    media_component_videomixer_args& pointer_args = request.sample.args->pointer_args;
    media_sample_video_mixer_frame frame, pointer_frame;

    try
    {
        this->capture_frame(frame.buffer, pointer_frame.buffer);
    }
    catch(displaycapture_exception)
    {
        this->set_broken();
    }

    // params are ignored if the buffer in sample is null(=silent)
    build_frame(frame);
    build_pointer_frame(pointer_frame);

    this->source_helper.add_new_sample(frame);
    this->source_pointer_helper.add_new_sample(pointer_frame);

    // frame position and frame_end must be the same for frame and pointer frame
    args.frame_end = frame_end;
    pointer_args.frame_end = frame_end;

    media_sample_video_mixer_frames_t sample = this->source_helper.make_sample(frame_end),
        pointer_sample = this->source_pointer_helper.make_sample(frame_end);

    if(args.sample)
        sample->move_frames_to(args.sample.get(), frame_end);
    else
        args.sample = sample;

    if(pointer_args.sample)
        pointer_sample->move_frames_to(pointer_args.sample.get(), frame_end);
    else
        pointer_args.sample = pointer_sample;
}

void source_displaycapture::dispatch(request_t& request)
{
    stream_displaycapture* stream = static_cast<stream_displaycapture*>(request.stream);

    this->session->give_sample(stream, request.sample.args.has_value() ?
        &request.sample.args->args : NULL, request.rp);
    this->session->give_sample(stream->pointer_stream.get(), request.sample.args.has_value() ?
        &request.sample.args->pointer_args : NULL, request.rp);
}

HRESULT source_displaycapture::initialize_pointer_texture(media_buffer_texture_t& pointer)
{
    D3D11_SUBRESOURCE_DATA init_data;

    switch(this->pointer_shape_info.Type)
    {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        pointer = this->acquire_buffer(this->available_pointer_samples);

        init_data.pSysMem = (const void*)&this->pointer_buffer[0];
        init_data.SysMemPitch = this->pointer_shape_info.Pitch;
        init_data.SysMemSlicePitch = 0;

        D3D11_TEXTURE2D_DESC desc;
        desc.Width = this->pointer_shape_info.Width;
        desc.Height = this->pointer_shape_info.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        pointer->initialize(this->d3d11dev2, desc, &init_data, true);

        break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        pointer = NULL;

        // predefined texture cannot really be used because the background for the pointer
        // might be a dark color

        break;
    default:
        pointer = NULL;
        break;
    }

    return S_OK;
}

HRESULT source_displaycapture::create_pointer_texture(
    const DXGI_OUTDUPL_FRAME_INFO& frame_info, media_buffer_texture_t& pointer)
{
    HRESULT hr = S_OK;

    // it is assumed that the lock is held while in this function

    if(frame_info.PointerShapeBufferSize)
    {
        this->pointer_buffer.resize(frame_info.PointerShapeBufferSize);
        UINT buffer_size_required;

        CHECK_HR(hr = this->output_duplication->GetFramePointerShape(
            (UINT)this->pointer_buffer.size(), (void*)&this->pointer_buffer[0], &buffer_size_required,
            &this->pointer_shape_info));
    }

    CHECK_HR(hr = this->initialize_pointer_texture(pointer));
done:
    return hr;
}

void source_displaycapture::capture_frame(media_buffer_texture_t& output_frame,
    media_buffer_texture_t& pointer_frame)
{
    assert_(!output_frame);
    assert_(!pointer_frame);

    // dxgi functions need to be synchronized with the context mutex;
    // lock always, since the output duplication seems to deadlock even if another device
    // is being used
    scoped_lock lock(*this->context_mutex);

    HRESULT hr = S_OK;
    CComPtr<IDXGIResource> frame;
    CComPtr<ID3D11Texture2D> screen_frame;
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    bool new_pointer_shape = false;
    
capture:
    frame = NULL;
    screen_frame = NULL;
    memset(&frame_info, 0, sizeof(DXGI_OUTDUPL_FRAME_INFO));

    if(!this->output_duplication || !this->output || FAILED(hr))
        CHECK_HR(hr = this->reinitialize(this->output_index));

    CHECK_HR(hr = this->output_duplication->AcquireNextFrame(0, &frame_info, &frame));
    CHECK_HR(hr = frame->QueryInterface(&screen_frame));

    D3D11_TEXTURE2D_DESC screen_frame_desc;
    output_frame = this->acquire_buffer(this->available_samples);
    screen_frame->GetDesc(&screen_frame_desc);
    screen_frame_desc.MiscFlags = 0;
    screen_frame_desc.Usage = D3D11_USAGE_DEFAULT;
    output_frame->initialize(this->d3d11dev2, screen_frame_desc, NULL);

    // pointer position update
    if(frame_info.LastMouseUpdateTime.QuadPart != 0)
        this->pointer_position = frame_info.PointerPosition;

    // if monochrome or masked pointer is used, the pointer texture needs to be recreated
    // every time
    new_pointer_shape = (frame_info.PointerShapeBufferSize != 0 ||
        this->pointer_shape_info.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR);
    if(new_pointer_shape)
    {
        // this function might set the texture to null
        CHECK_HR(hr = this->create_pointer_texture(frame_info, pointer_frame));
        this->newest_pointer_buffer = pointer_frame;
    }

    // copy;
    // the last present time might be 0 on the first call aswell,
    // so checking for the newest buffer is necessary;
    // newest pointer shape on the other hand seems to be true on the first call
    if(frame_info.LastPresentTime.QuadPart != 0 || !this->newest_buffer)
    {
        if(this->same_adapter)
            this->d3d11devctx->CopyResource(output_frame->texture, screen_frame);
        else
            CHECK_HR(hr = this->copy_between_adapters(
                this->d3d11dev2, output_frame->texture, this->d3d11dev, screen_frame));

        this->newest_buffer = output_frame;
    }

done:
    if(this->output_duplication)
        this->output_duplication->ReleaseFrame();

    if(FAILED(hr))
    {
        if(hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            output_frame = this->newest_buffer;
            pointer_frame = this->newest_pointer_buffer;

            return;
        }
        else if(hr == DXGI_ERROR_ACCESS_LOST)
            goto capture;
        else if((hr == E_ACCESSDENIED && !this->output_duplication) || hr == DXGI_ERROR_UNSUPPORTED ||
            hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || hr == DXGI_ERROR_SESSION_DISCONNECTED)
        {
            output_frame = this->newest_buffer;
            pointer_frame = this->newest_pointer_buffer;

            return;
        }
        else
        {
            output_frame = this->newest_buffer;
            pointer_frame = this->newest_pointer_buffer;

            throw displaycapture_exception();
        }
    }

    if(frame_info.LastPresentTime.QuadPart == 0)
        output_frame = this->newest_buffer;
    if(!new_pointer_shape)
        pointer_frame = this->newest_pointer_buffer;
}

media_buffer_texture_t source_displaycapture::acquire_buffer(const std::shared_ptr<buffer_pool>& pool)
{
    buffer_pool::scoped_lock lock(pool->mutex);
    return pool->acquire_buffer();
}

HRESULT source_displaycapture::reinitialize(UINT output_index)
{
    if(output_index == (UINT)-1)
        throw HR_EXCEPTION(E_UNEXPECTED);

    static const DXGI_FORMAT supported_formats[] =
    {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        /*DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,*/
    };

    HRESULT hr = S_OK;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<IDXGIAdapter1> dxgiadapter;
    CComPtr<IDXGIOutput> output;

    // get dxgi dev
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&dxgidev));

    // get dxgi adapter
    CHECK_HR(hr = dxgidev->GetParent(__uuidof(IDXGIAdapter1), (void**)&dxgiadapter));

    // get the output
    CHECK_HR(hr = dxgiadapter->EnumOutputs(output_index, &output));
    DXGI_OUTPUT_DESC desc;
    CHECK_HR(hr = output->GetDesc(&desc));

    // qi for output5
    this->output = NULL;
    CHECK_HR(hr = output->QueryInterface(&this->output));

    // create the desktop duplication;
    // duplicateoutput1 works if the dpi awareness of the process is per monitor aware v2
    this->output_duplication = NULL;
    /*CHECK_HR(hr = this->output->DuplicateOutput(this->d3d11dev, &this->output_duplication));*/
    CHECK_HR(hr = this->output->DuplicateOutput1(this->d3d11dev, 0,
        ARRAYSIZE(supported_formats), supported_formats, &this->output_duplication));

    this->output_duplication->GetDesc(&this->outdupl_desc);

done:
    return hr;
}

void source_displaycapture::initialize(
    const control_class_t& ctrl_pipeline,
    UINT output_index,
    const CComPtr<ID3D11Device>& d3d11dev,
    const CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr = S_OK;

    this->source_base::initialize(ctrl_pipeline,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);

    this->ctrl_pipeline = ctrl_pipeline;
    this->d3d11dev2 = this->d3d11dev = d3d11dev;
    this->d3d11devctx2 = this->d3d11devctx = devctx;
    this->output_index = output_index;
    this->same_adapter = true;

    CHECK_HR(hr = this->reinitialize(output_index));

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
    }
}

void source_displaycapture::initialize(
    const control_class_t& ctrl_pipeline,
    UINT adapter_index,
    UINT output_index,
    const CComPtr<IDXGIFactory1>& factory,
    const CComPtr<ID3D11Device>& d3d11dev,
    const CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr = S_OK;

    this->source_base::initialize(ctrl_pipeline,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);

    CComPtr<IDXGIAdapter1> dxgiadapter;
    D3D_FEATURE_LEVEL feature_level;
    D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };

    CHECK_HR(hr = factory->EnumAdapters1(adapter_index, &dxgiadapter));
    CHECK_HR(hr = D3D11CreateDevice(
        dxgiadapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT | CREATE_DEVICE_DEBUG,
        feature_levels, ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION, &this->d3d11dev, &feature_level, &this->d3d11devctx));

    // currently initialization never fails
    this->initialize(ctrl_pipeline, output_index, this->d3d11dev, this->d3d11devctx);

    this->d3d11dev2 = d3d11dev;
    this->d3d11devctx2 = devctx;
    this->same_adapter = false;

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
    }
    else if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

HRESULT source_displaycapture::copy_between_adapters(
    ID3D11Device* dst_dev, ID3D11Texture2D* dst,
    ID3D11Device* src_dev, ID3D11Texture2D* src)
{
    HRESULT hr = S_OK;
    D3D11_TEXTURE2D_DESC desc;
    CComPtr<ID3D11DeviceContext> src_dev_ctx, dst_dev_ctx;

    src_dev->GetImmediateContext(&src_dev_ctx);
    dst_dev->GetImmediateContext(&dst_dev_ctx);

    src->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;

    // create staging texture for reading
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if(!this->stage_src)
        CHECK_HR(hr = src_dev->CreateTexture2D(&desc, NULL, &this->stage_src));

    // create staging texture for writing
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if(!this->stage_dst)
        CHECK_HR(hr = dst_dev->CreateTexture2D(&desc, NULL, &this->stage_dst));

    // copy src video memory to src staging buffer
    src_dev_ctx->CopyResource(this->stage_src, src);

    // map the src staging buffer for reading
    D3D11_MAPPED_SUBRESOURCE src_sub_rsrc;
    src_dev_ctx->Map(this->stage_src, 0, D3D11_MAP_READ, 0, &src_sub_rsrc);

    // map the dst staging buffer for writing
    D3D11_MAPPED_SUBRESOURCE dst_sub_rsrc;
    {
        // context mutex needs to be locked for the whole duration of map/unmap
        scoped_lock lock(*this->context_mutex);
        dst_dev_ctx->Map(this->stage_dst, 0, D3D11_MAP_WRITE, 0, &dst_sub_rsrc);

        assert_(src_sub_rsrc.RowPitch == dst_sub_rsrc.RowPitch);

        // copy from src staging buffer to dst staging buffer
        const size_t size = dst_sub_rsrc.RowPitch * desc.Height;
        memcpy(dst_sub_rsrc.pData, src_sub_rsrc.pData, size);

        // unmap the staging buffers
        src_dev_ctx->Unmap(this->stage_src, 0);
        dst_dev_ctx->Unmap(this->stage_dst, 0);

        // copy from dst staging buffer to dst video memory
        dst_dev_ctx->CopyResource(dst, this->stage_dst);
    }

done:
    return hr;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture::stream_displaycapture(const source_displaycapture_t& source) :
    stream_source_base(source),
    source(source)
{
}

void stream_displaycapture::on_component_start(time_unit t)
{
    this->source->source_helper.initialize(convert_to_frame_unit(t,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den));
    this->source->source_pointer_helper.initialize(convert_to_frame_unit(t,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture_pointer::stream_displaycapture_pointer(const source_displaycapture_t& source) :
    source(source)
{
}

media_stream::result_t stream_displaycapture_pointer::request_sample(
    const request_packet&, const media_stream*)
{
    // do nothing; the process sample is called from the stream displaycapture
    return OK;
}

media_stream::result_t stream_displaycapture_pointer::process_sample(
    const media_component_args*, const request_packet&, const media_stream*)
{
    // do nothing; the process sample is called from the stream displaycapture
    return OK;
}