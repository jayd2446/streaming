#include "source_displaycapture5.h"
#include "presentation_clock.h"
#include <iostream>
#include <atomic>
#include <dxgi1_5.h>
#include <Mferror.h>

#pragma comment(lib, "D3D11.lib")
#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
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


// TODO: decide if allocating memory for newest buffer is redundant
// (currently is not, since the videprocessor assumes a non null sample view)

source_displaycapture5::source_displaycapture5(
    const media_session_t& session, context_mutex_t context_mutex) : 
    media_source(session),
    newest_buffer(new media_buffer_lockable_texture),
    newest_pointer_buffer(new media_buffer_lockable_texture),
    context_mutex(context_mutex),
    output_index((UINT)-1)
{
    this->pointer_position.Visible = FALSE;
    this->pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
}

stream_displaycapture5_t source_displaycapture5::create_stream(const stream_videoprocessor_controller_t& c)
{
    return stream_displaycapture5_t(
        new stream_displaycapture5(this->shared_from_this<source_displaycapture5>(), c));
}

stream_displaycapture5_pointer_t source_displaycapture5::create_pointer_stream()
{
    return stream_displaycapture5_pointer_t(
        new stream_displaycapture5_pointer(this->shared_from_this<source_displaycapture5>()));
}

HRESULT source_displaycapture5::reinitialize(UINT output_index)
{
    if(output_index == (UINT)-1)
        throw std::exception();

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

done:
    return hr;
}

void source_displaycapture5::initialize(
    const control_pipeline_t& ctrl_pipeline,
    UINT output_index, 
    const CComPtr<ID3D11Device>& d3d11dev, 
    const CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr = S_OK;

    this->ctrl_pipeline = ctrl_pipeline;
    this->d3d11dev2 = this->d3d11dev = d3d11dev;
    this->d3d11devctx2 = this->d3d11devctx = devctx;
    this->output_index = output_index;

    CHECK_HR(hr = this->reinitialize(output_index));

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
        /*return hr;*/
    }
    /*else if(FAILED(hr))
        throw std::exception();*/
}

void source_displaycapture5::initialize(
    const control_pipeline_t& ctrl_pipeline,
    UINT adapter_index,
    UINT output_index, 
    const CComPtr<IDXGIFactory1>& factory,
    const CComPtr<ID3D11Device>& d3d11dev,
    const CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr = S_OK;
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

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
    }
    else if(FAILED(hr))
        throw std::exception();
}
HRESULT source_displaycapture5::setup_initial_data(const media_buffer_texture_t& pointer)
{
    HRESULT hr = S_OK;
    D3D11_SUBRESOURCE_DATA init_data;

    switch(this->pointer_shape_info.Type)
    {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        pointer->texture = NULL;

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
        CHECK_HR(hr = this->d3d11dev2->CreateTexture2D(&desc, &init_data, &pointer->texture));
        break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        pointer->texture = NULL;

        // predefined texture cannot really be used because the background for the pointer
        // might be a dark color
        
        break;
    default:
        pointer->texture = NULL;
        break;
    }

done:
    return hr;
}

HRESULT source_displaycapture5::create_pointer_texture(
    const DXGI_OUTDUPL_FRAME_INFO& frame_info, const media_buffer_texture_t& pointer)
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

    CHECK_HR(hr = this->setup_initial_data(pointer));
done:
    return hr;
}

bool source_displaycapture5::capture_frame(
    bool& new_pointer_shape,
    DXGI_OUTDUPL_POINTER_POSITION& pointer_position,
    media_buffer_lockable_texture_t& pointer,
    media_buffer_lockable_texture_t& buffer,
    time_unit& timestamp, 
    const presentation_clock_t& clock)
{
    // TODO: context mutex not needed when the dxgioutput is initialized with another device

    // dxgi functions need to be synchronized with the context mutex
    scoped_lock lock(*this->context_mutex);

    CComPtr<IDXGIResource> frame;
    CComPtr<ID3D11Texture2D> screen_frame;
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    HRESULT hr = S_OK;

    // if new frame: unused buffer will save the new frame, newest buffer is set to point to unused buffer,
    // unused buffer is swapped with parameter buffer
    // if no new frame: the unused and new buffer will remain the same

    // update the pointer position beforehand because acquirenextframe might return timeout error
    pointer_position = this->pointer_position;
    new_pointer_shape = false;

capture:
    frame = NULL;
    screen_frame = NULL;
    memset(&frame_info, 0, sizeof(DXGI_OUTDUPL_FRAME_INFO));

    if(!this->output_duplication || !this->output || FAILED(hr))
        CHECK_HR(hr = this->reinitialize(this->output_index));

    CHECK_HR(hr = this->output_duplication->AcquireNextFrame(0, &frame_info, &frame));
    CHECK_HR(hr = frame->QueryInterface(&screen_frame));

    if(!buffer->texture)
    {
        D3D11_TEXTURE2D_DESC screen_frame_desc;
        CComPtr<IDXGIResource> idxgiresource;
        HANDLE handle;
        const bool same_device = this->d3d11dev2.IsEqualObject(this->d3d11dev);

        screen_frame->GetDesc(&screen_frame_desc);
        screen_frame_desc.MiscFlags = same_device ? 0 : D3D11_RESOURCE_MISC_SHARED;
        screen_frame_desc.Usage = D3D11_USAGE_DEFAULT;

        buffer->intermediate_texture = NULL;
        CHECK_HR(hr = this->d3d11dev->CreateTexture2D(
            &screen_frame_desc, NULL, &buffer->intermediate_texture));
        
        if(!same_device)
        {
            CHECK_HR(hr = buffer->intermediate_texture->QueryInterface(&idxgiresource));
            CHECK_HR(hr = idxgiresource->GetSharedHandle(&handle));
            CHECK_HR(hr = this->d3d11dev2->OpenSharedResource(
                handle,  __uuidof(ID3D11Texture2D), (void**)&buffer->texture));
        }
        else
            buffer->texture = buffer->intermediate_texture;
    }

    // pointer position update
    if(frame_info.LastMouseUpdateTime.QuadPart != 0)
    {
        this->pointer_position = frame_info.PointerPosition;
        pointer_position = this->pointer_position;
    }

    // if monochrome or masked pointer is used, the pointer texture needs to be recreated
    // every time
    new_pointer_shape = (frame_info.PointerShapeBufferSize != 0 || 
        this->pointer_shape_info.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR);
    if(new_pointer_shape)
    {
        CHECK_HR(hr = this->create_pointer_texture(frame_info, pointer));
        this->newest_pointer_buffer = pointer;
        /*std::atomic_exchange(&this->newest_pointer_buffer, pointer);*/
    }

    // copy
    if(frame_info.LastPresentTime.QuadPart != 0)
    {
        this->d3d11devctx->CopyResource(buffer->intermediate_texture, screen_frame);

        /*this->d3d11devctx->Flush();*/

        // TODO: the timestamp might not be consecutive
        /*timestamp = clock->performance_counter_to_time_unit(frame_info.LastPresentTime);*/

        // update the newest sample
        this->newest_buffer = buffer;
        /*std::atomic_exchange(&this->newest_buffer, buffer);*/
    }
    else
        /*timestamp = clock->get_current_time()*/;

done:
    if(this->output_duplication)
        this->output_duplication->ReleaseFrame();

    if(FAILED(hr))
    {
        if(hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            /*std::cout << "FRAME IS NULL------------------" << std::endl;*/
            /*timestamp = clock->get_current_time();*/
            return false;
        }
        else if(hr == DXGI_ERROR_ACCESS_LOST)
            goto capture;
        else if((hr == E_ACCESSDENIED && !this->output_duplication) || hr == DXGI_ERROR_UNSUPPORTED ||
            hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || hr == DXGI_ERROR_SESSION_DISCONNECTED)
            return false;
        else
            throw displaycapture_exception();
    }

    return (frame_info.LastPresentTime.QuadPart != 0);
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture5::stream_displaycapture5(const source_displaycapture5_t& source, 
    const stream_videoprocessor_controller_t& videoprocessor_controller) : 
    source(source),
    buffer(new media_buffer_lockable_texture),
    videoprocessor_controller(videoprocessor_controller)
{
    this->capture_frame_callback.Attach(
        new async_callback_t(&stream_displaycapture5::capture_frame_cb));
}

void stream_displaycapture5::capture_frame_cb(void*)
{
    // there exists a possibility for dead lock if another thread tries to read
    // the cached texture, and this thread has already locked the sample;
    // unlocking the capture frame mutex must be ensured before trying to lock the
    // cache texture
    std::unique_lock<std::recursive_mutex> lock(this->source->capture_frame_mutex);

    stream_videoprocessor_controller::params_t params;
    this->videoprocessor_controller->get_params(params);
    // enable alpha is only for pointer which has alpha values
    params.enable_alpha = false;
    media_sample_videoprocessor sample(params, this->buffer->lock_buffer());
    /*media_sample_view_videoprocessor sample_view(this->buffer);*/
    /*sample_view.params = params;*/
    media_sample_videoprocessor pointer_sample(this->pointer_stream->buffer->lock_buffer());
    /*media_sample_view_videoprocessor pointer_sample_view(this->pointer_stream->buffer);*/

    /*std::unique_lock<std::recursive_mutex> lock(this->source->capture_frame_mutex);*/

    bool frame_captured, new_pointer_shape;
    DXGI_OUTDUPL_POINTER_POSITION pointer_position;
    time_unit timestamp;
    presentation_clock_t clock;

    // capture a frame
    // TODO: buffer parameter is redundant because the routine
    // updates the newest buffer field

    // clock is assumed to be valid
    this->rp.get_clock(clock);
    try
    {
        frame_captured = this->source->capture_frame(
            new_pointer_shape, pointer_position, this->pointer_stream->buffer,
            this->buffer, timestamp, clock);
    }
    catch(displaycapture_exception)
    {
        frame_captured = false;
        new_pointer_shape = false;
    }

    if(!frame_captured)
    {
        // sample view must be reset to null before assigning a new sample view,
        // that is because the media_sample_view would lock the sample before
        // sample_view releasing its own reference to another sample_view
        sample.buffer = NULL;
        sample.buffer = this->source->newest_buffer->lock_buffer(buffer_lock_t::READ_LOCK);
    }
    else
        this->buffer->unlock_write();

    if(!new_pointer_shape)
    {
        pointer_sample.buffer = NULL;
        pointer_sample.buffer = 
            this->source->newest_pointer_buffer->lock_buffer(buffer_lock_t::READ_LOCK);
    }
    else
        this->pointer_stream->buffer->unlock_write();

    request_packet rp = this->rp;
    this->rp = request_packet();

    D3D11_TEXTURE2D_DESC desc;
    D3D11_TEXTURE2D_DESC* ptr_desc = NULL;
    if(sample.buffer->texture)
        sample.buffer->texture->GetDesc(ptr_desc = &desc);

    sample.timestamp = rp.request_time;

    lock.unlock();

    this->process_sample(sample, rp, NULL);
    this->pointer_stream->dispatch(
        new_pointer_shape, pointer_position, ptr_desc, pointer_sample, rp);
}

media_stream::result_t stream_displaycapture5::request_sample(request_packet& rp, const media_stream*)
{
    this->rp = rp;

    // dispatch the capture request
    const HRESULT hr = this->capture_frame_callback->mf_put_work_item(
        this->shared_from_this<stream_displaycapture5>());
    if(FAILED(hr))
        this->rp = request_packet();
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
    else if(hr == MF_E_SHUTDOWN)
        return FATAL_ERROR;

    return OK;
}

media_stream::result_t stream_displaycapture5::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream*)
{
    this->source->session->give_sample(this, sample_view, rp, true);
    return OK;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture5_pointer::stream_displaycapture5_pointer(const source_displaycapture5_t& source) :
    source(source),
    buffer(new media_buffer_lockable_texture),
    null_buffer(new media_buffer_lockable_texture)
{
}

void stream_displaycapture5_pointer::dispatch(
    bool new_pointer_shape, 
    const DXGI_OUTDUPL_POINTER_POSITION& pointer_position, 
    const D3D11_TEXTURE2D_DESC* desktop_desc,
    media_sample_videoprocessor& sample_view,
    request_packet& rp)
{
    if(pointer_position.Visible && desktop_desc && sample_view.buffer->texture)
    {
        D3D11_TEXTURE2D_DESC desc;
        sample_view.buffer->texture->GetDesc(&desc);

        sample_view.params.enable_alpha = true;
        sample_view.params.source_rect.left = sample_view.params.source_rect.top = 0;
        sample_view.params.source_rect.right = desc.Width;
        sample_view.params.source_rect.bottom = desc.Height;
        sample_view.params.dest_rect.left = pointer_position.Position.x;
        sample_view.params.dest_rect.top = pointer_position.Position.y;
        sample_view.params.dest_rect.right = 
            sample_view.params.dest_rect.left + desc.Width;
        sample_view.params.dest_rect.bottom = 
            sample_view.params.dest_rect.top + desc.Height;
    }
    else
    {
        sample_view.buffer = this->null_buffer;
        /*sample_view.attach(this->null_buffer, view_lock_t::READ_LOCK_BUFFERS);*/
    }

    sample_view.timestamp = rp.request_time;

    this->source->session->give_sample(this, sample_view, rp, true);
}

media_stream::result_t stream_displaycapture5_pointer::request_sample(request_packet&, const media_stream*)
{
    // do nothing; the process sample is called from the stream displaycapture
    return OK;
}

media_stream::result_t stream_displaycapture5_pointer::process_sample(
    const media_sample&, request_packet&, const media_stream*)
{
    assert_(false);
    return OK;
}