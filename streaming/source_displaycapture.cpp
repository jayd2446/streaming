#include "source_displaycapture.h"
#include "control_pipeline2.h"
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
    same_device(false),
    available_samples(new buffer_pool),
    available_pointer_samples(new buffer_pool),
    buffer_pool_video_frames(new buffer_pool_video_frames_t)
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
    {
        buffer_pool_video_frames_t::scoped_lock lock(this->buffer_pool_video_frames->mutex);
        this->buffer_pool_video_frames->dispose();
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

bool source_displaycapture::get_samples_end(const request_t& request, frame_unit& end)
{
    // note: time shifting isn't possible here, unless the drain is properly handled aswell

    // displaycapture just pretends that it has samples up to the request point
    end = convert_to_frame_unit(request.rp.request_time,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);

    return true;
}

void source_displaycapture::make_request(request_t& request, frame_unit frame_end)
{
    // capture the frame and populate the request

    request.sample.args = std::make_optional<media_component_videomixer_args>();
    request.sample.pointer_args = std::make_optional<media_component_videomixer_args>();

    media_component_videomixer_args& args = *request.sample.args;
    media_component_videomixer_args& pointer_args = *request.sample.pointer_args;
    media_sample_video_mixer_frame frame, pointer_frame;

    auto partially_build_frame = [this](media_sample_video_mixer_frame& frame)
    {
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
    auto partially_build_pointer_frame = [this](media_sample_video_mixer_frame& pointer_frame)
    {
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

    {
        buffer_pool_video_frames_t::scoped_lock lock(this->buffer_pool_video_frames->mutex);
        args.sample = this->buffer_pool_video_frames->acquire_buffer();
        args.sample->initialize();

        pointer_args.sample = this->buffer_pool_video_frames->acquire_buffer();
        pointer_args.sample->initialize();
    }

    // frame position and frame_end must be the same for frame and pointer frame
    args.frame_end = frame_end;
    frame.dur = 1;
    frame.pos = args.frame_end - frame.dur;

    pointer_args.frame_end = frame_end;
    pointer_frame.dur = 1;
    pointer_frame.pos = pointer_args.frame_end - pointer_frame.dur;

    // add padding for every skipped frame;
    // the padded frames will use the same user videomixer parameters as the newest frame that is
    // being served
    const frame_unit skipped_frames_dur = (frame.pos - this->last_captured_frame_end);
    if(skipped_frames_dur > 0)
    {
        media_sample_video_mixer_frame duplicate_frame;
        duplicate_frame.pos = this->last_captured_frame_end;
        duplicate_frame.dur = skipped_frames_dur;
        duplicate_frame.buffer = this->newest_buffer;
        partially_build_frame(duplicate_frame);
        args.sample->frames.push_back(duplicate_frame);

        media_sample_video_mixer_frame duplicate_pointer_frame;
        duplicate_pointer_frame.pos = this->last_captured_frame_end;
        duplicate_pointer_frame.dur = skipped_frames_dur;
        duplicate_pointer_frame.buffer = this->newest_pointer_buffer;
        partially_build_pointer_frame(duplicate_pointer_frame);
        pointer_args.sample->frames.push_back(duplicate_pointer_frame);
    }

    this->last_captured_frame_end = frame_end;

    try
    {
        this->capture_frame(frame.buffer, pointer_frame.buffer);
    }
    catch(displaycapture_exception)
    {
        // reset the scene to recreate this component
        this->request_reinitialization(this->ctrl_pipeline);
    }

    // params in args are ignored if the buffer in sample is null(=silent)
    partially_build_frame(frame);
    partially_build_pointer_frame(pointer_frame);

    args.sample->frames.push_back(frame);
    args.sample->end = args.frame_end;

    pointer_args.sample->frames.push_back(pointer_frame);
    pointer_args.sample->end = pointer_args.frame_end;

    // keep the frames buffer within the limits
    assert_(!args.sample->frames.empty());
    assert_(!pointer_args.sample->frames.empty());

    const bool limit_reached = 
        args.sample->move_frames_to(NULL, args.sample->end - maximum_buffer_size);
    const bool limit_reached2 =
        pointer_args.sample->move_frames_to(NULL, pointer_args.sample->end - maximum_buffer_size);

    if(limit_reached || limit_reached2)
    {
        std::cout << "source_displaycapture buffer limit reached, excess frames discarded" << std::endl;
    }
}

void source_displaycapture::dispatch(request_t& request)
{
    stream_displaycapture* stream = static_cast<stream_displaycapture*>(request.stream);

    assert_(request.sample.args.has_value());
    assert_(request.sample.pointer_args.has_value());

    this->session->give_sample(stream, request.sample.args.has_value() ?
        &(*request.sample.args) : NULL, request.rp);
    this->session->give_sample(stream->pointer_stream.get(), request.sample.pointer_args.has_value() ?
        &(*request.sample.pointer_args) : NULL, request.rp);
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
        if(this->same_device)
            this->d3d11devctx->CopyResource(output_frame->texture, screen_frame);
        else
            // TODO: copy_between_adapters
            assert_(false);

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
    // TODO: this probably should be a free function that is defined in buffer_pool
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

    this->source_base::initialize(
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);

    this->ctrl_pipeline = ctrl_pipeline;
    this->d3d11dev2 = this->d3d11dev = d3d11dev;
    this->d3d11devctx2 = this->d3d11devctx = devctx;
    this->output_index = output_index;
    this->same_device = true;

    CHECK_HR(hr = this->reinitialize(output_index));

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
    }
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
    this->source->last_captured_frame_end = convert_to_frame_unit(t,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);
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