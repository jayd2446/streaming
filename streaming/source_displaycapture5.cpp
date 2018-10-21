#include "source_displaycapture5.h"
#include "presentation_clock.h"
#include "control_pipeline2.h"
#include <iostream>
#include <atomic>
#include <dxgi1_5.h>
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


// TODO: decide if allocating memory for newest buffer is redundant
// (currently is not, since the videprocessor assumes a non null sample view)

source_displaycapture5::source_displaycapture5(
    const media_session_t& session, context_mutex_t context_mutex) : 
    media_source(session),
    newest_buffer(new media_buffer_texture),
    newest_pointer_buffer(new media_buffer_texture),
    available_samples(new buffer_pool),
    available_pointer_samples(new buffer_pool),
    context_mutex(context_mutex),
    output_index((UINT)-1),
    same_device(false),
    last_timestamp2(std::numeric_limits<time_unit>::min())
{
    this->pointer_position.Visible = FALSE;
    this->pointer_shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    this->last_timestamp.QuadPart = std::numeric_limits<LONGLONG>::min();
}

source_displaycapture5::~source_displaycapture5()
{
    // clear the container so that the cyclic dependency between the container and its
    // elements is broken
    {
        std::atomic_exchange(&this->newest_buffer, media_buffer_texture_t());

        buffer_pool::scoped_lock lock(this->available_samples->mutex);
        this->available_samples->dispose();
    }
    {
        std::atomic_exchange(&this->newest_pointer_buffer, media_buffer_texture_t());

        buffer_pool::scoped_lock lock(this->available_pointer_samples->mutex);
        this->available_pointer_samples->dispose();
    }
}

stream_displaycapture5_t source_displaycapture5::create_stream()
{
    return stream_displaycapture5_t(
        new stream_displaycapture5(this->shared_from_this<source_displaycapture5>()));
}

stream_displaycapture5_pointer_t source_displaycapture5::create_pointer_stream()
{
    return stream_displaycapture5_pointer_t(
        new stream_displaycapture5_pointer(this->shared_from_this<source_displaycapture5>()));
}

HRESULT source_displaycapture5::reinitialize(UINT output_index)
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

void source_displaycapture5::initialize(
    const control_class_t& ctrl_pipeline,
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

    this->same_device = true;

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
        /*return hr;*/
    }
    /*else if(FAILED(hr))
        throw HR_EXCEPTION(hr);*/
}

void source_displaycapture5::initialize(
    const control_class_t& ctrl_pipeline,
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

    this->same_device = false;

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
    }
    else if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

HRESULT source_displaycapture5::copy_between_adapters(
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
    media_buffer_texture_t& pointer,
    media_buffer_texture_t& buffer,
    time_unit& timestamp, 
    const presentation_clock_t& clock)
{
    // dxgi functions need to be synchronized with the context mutex
    // just lock always, because the output duplication seems to deadlock otherwise
    std::unique_lock<std::recursive_mutex> lock(*this->context_mutex/*, std::defer_lock*/);
    /*if(this->same_device)
        lock.lock();*/

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

    if(frame_info.LastPresentTime.QuadPart != 0)
    {
        if(frame_info.LastPresentTime.QuadPart <= this->last_timestamp.QuadPart)
        {
            std::cout << "timestamp error in source_displaycapture5::capture_frame" << std::endl;
            assert_(false);
        }
        this->last_timestamp = frame_info.LastPresentTime;
    }

    if(!buffer->texture)
    {
        D3D11_TEXTURE2D_DESC screen_frame_desc;

        screen_frame->GetDesc(&screen_frame_desc);
        screen_frame_desc.MiscFlags = 0;
        screen_frame_desc.Usage = D3D11_USAGE_DEFAULT;

        CHECK_HR(hr = this->d3d11dev2->CreateTexture2D(&screen_frame_desc, NULL, &buffer->texture));
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
        std::atomic_exchange(&this->newest_pointer_buffer, pointer);
    }

    // copy
    if(frame_info.LastPresentTime.QuadPart != 0)
    {
        if(this->same_device)
            this->d3d11devctx->CopyResource(buffer->texture, screen_frame);
        else
            this->copy_between_adapters(
                this->d3d11dev2, buffer->texture, this->d3d11dev, screen_frame);

        // update the newest sample
        std::atomic_exchange(&this->newest_buffer, buffer);
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


stream_displaycapture5::stream_displaycapture5(const source_displaycapture5_t& source) : 
    source(source)
{
    this->capture_frame_callback.Attach(
        new async_callback_t(&stream_displaycapture5::capture_frame_cb));
}

void stream_displaycapture5::capture_frame_cb(void*)
{
    std::unique_lock<std::recursive_mutex> lock(this->source->capture_frame_mutex);
    media_sample_videoprocessor sample;
    media_sample_videoprocessor pointer_sample;
    {
        source_displaycapture5::buffer_pool::scoped_lock lock(this->source->available_samples->mutex);
        if(this->source->available_samples->container.empty())
        {
            media_buffer_pooled_texture_t pooled_buffer(new media_buffer_pooled_texture(
                this->source->available_samples));
            sample.buffer = pooled_buffer->create_pooled_buffer();
            /*std::cout << "creating new..." << std::endl;*/
        }
        else
        {
            sample.buffer = this->source->available_samples->container.top()->create_pooled_buffer();
            this->source->available_samples->container.pop();
            /*std::cout << "reusing..." << std::endl;*/
        }
    }
    {
        source_displaycapture5::buffer_pool::scoped_lock 
            lock(this->source->available_pointer_samples->mutex);
        if(this->source->available_pointer_samples->container.empty())
        {
            media_buffer_pooled_texture_t pooled_buffer(new media_buffer_pooled_texture(
                this->source->available_pointer_samples));
            pointer_sample.buffer = pooled_buffer->create_pooled_buffer();
            /*std::cout << "creating new..." << std::endl;*/
        }
        else
        {
            pointer_sample.buffer = 
                this->source->available_pointer_samples->container.top()->create_pooled_buffer();
            this->source->available_pointer_samples->container.pop();
            /*std::cout << "reusing..." << std::endl;*/
        }
    }

    bool frame_captured, new_pointer_shape;
    DXGI_OUTDUPL_POINTER_POSITION pointer_position;
    time_unit timestamp;
    presentation_clock_t clock;
    request_packet rp;

    {
        scoped_lock lock(this->source->requests_mutex);
        rp = this->source->requests.front();
        this->source->requests.pop();
    }

    if(rp.request_time <= this->source->last_timestamp2)
    {
        std::cout << "timestamp error in stream_displaycapture5::capture_frame_cb" << std::endl;
        assert_(false);
    }
    this->source->last_timestamp2 = rp.request_time;

    // capture a frame
    // TODO: buffer parameter is redundant because the routine
    // updates the newest buffer field

    // clock is assumed to be valid
    rp.get_clock(clock);
    try
    {
        frame_captured = this->source->capture_frame(
            new_pointer_shape, pointer_position, pointer_sample.buffer,
            sample.buffer, timestamp, clock);
    }
    catch(displaycapture_exception)
    {
        frame_captured = false;
        new_pointer_shape = false;

        // reset the scene to recreate this component
        // TODO: make sure that no deadlocks occur
        this->source->request_reinitialization(this->source->ctrl_pipeline);
    }

    if(!frame_captured)
        sample.buffer = this->source->newest_buffer;

    if(!new_pointer_shape)
        pointer_sample.buffer = this->source->newest_pointer_buffer;

    D3D11_TEXTURE2D_DESC desc;
    D3D11_TEXTURE2D_DESC* ptr_desc = NULL;
    if(sample.buffer->texture)
    {
        sample.buffer->texture->GetDesc(ptr_desc = &desc);

        // enable alpha is only for pointer which has alpha values
        sample.params.enable_alpha = false;
        sample.params.source_rect.top = sample.params.source_rect.left = 0;
        sample.params.source_rect.right = desc.Width;
        sample.params.source_rect.bottom = desc.Height;
        sample.params.dest_rect = sample.params.source_rect;
    }

    sample.timestamp = rp.request_time;

    lock.unlock();

    this->process_sample(sample, rp, NULL);
    this->pointer_stream->dispatch(
        new_pointer_shape, pointer_position, ptr_desc, pointer_sample, rp);
}

media_stream::result_t stream_displaycapture5::request_sample(request_packet& rp, const media_stream*)
{
    {
        scoped_lock lock(this->source->requests_mutex);
        this->source->requests.push(rp);
    }

    // dispatch the capture request
    const HRESULT hr = this->capture_frame_callback->mf_put_work_item(
        this->shared_from_this<stream_displaycapture5>());
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
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
    null_buffer(new media_buffer_texture)
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