#include "source_displaycapture5.h"
#include "presentation_clock.h"
#include <iostream>
#include <atomic>
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

extern LARGE_INTEGER pc_frequency;

// TODO: decide if allocating memory for newest buffer is redundant
// (currently is not, since the videprocessor assumes a non null sample view)

source_displaycapture5::source_displaycapture5(
    const media_session_t& session, context_mutex_t context_mutex) : 
    media_source(session),
    newest_buffer(new media_buffer_texture),
    newest_pointer_buffer(new media_buffer_texture),
    context_mutex(context_mutex)
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

HRESULT source_displaycapture5::initialize(
    UINT output_index, const CComPtr<ID3D11Device>& d3d11dev, const CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<IDXGIAdapter1> dxgiadapter;
    CComPtr<IDXGIOutput> output;

    this->d3d11dev = d3d11dev;
    this->d3d11devctx = devctx;

    // get dxgi dev
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&dxgidev));

    // get dxgi adapter
    CHECK_HR(hr = dxgidev->GetParent(__uuidof(IDXGIAdapter1), (void**)&dxgiadapter));

    // get the primary output
    CHECK_HR(hr = dxgiadapter->EnumOutputs(output_index, &output));
    DXGI_OUTPUT_DESC desc;
    CHECK_HR(hr = output->GetDesc(&desc));

    // qi for output1
    CHECK_HR(hr = output->QueryInterface(&this->output));

    // create the desktop duplication
    this->output_duplication = NULL;
    CHECK_HR(hr = this->output->DuplicateOutput(this->d3d11dev, &this->output_duplication));

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
        return hr;
    }
    else if(FAILED(hr))
        throw std::exception();

    return hr;
}

HRESULT source_displaycapture5::initialize(
    UINT adapter_index,
    UINT output_index, 
    const CComPtr<IDXGIFactory1>& factory,
    const CComPtr<ID3D11Device>& d3d11dev,
    const CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr = S_OK;
    CComPtr<IDXGIAdapter1> dxgiadapter;

    CHECK_HR(hr = factory->EnumAdapters1(adapter_index, &dxgiadapter));
    CHECK_HR(hr = D3D11CreateDevice(
        dxgiadapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT | CREATE_DEVICE_DEBUG,
        NULL, 0, D3D11_SDK_VERSION, &this->d3d11dev2, NULL, &this->d3d11devctx2));

    this->initialize(output_index, this->d3d11dev2, this->d3d11devctx2);
    this->d3d11dev = d3d11dev;
    this->d3d11devctx = devctx;

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
        return hr;
    }
    else if(FAILED(hr))
        throw std::exception();

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
        CHECK_HR(hr = this->d3d11dev->CreateTexture2D(&desc, &init_data, &pointer->texture));
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
    // TODO: context mutex not needed when the dxgioutput is initialized with another device

    // dxgi functions need to be synchronized with the context mutex
    scoped_lock lock(*this->context_mutex);

    CComPtr<IDXGIResource> frame;
    CComPtr<ID3D11Texture2D> screen_frame;
    DXGI_OUTDUPL_FRAME_INFO frame_info = {0};
    HRESULT hr = S_OK;

    // if new frame: unused buffer will save the new frame, newest buffer is set to point to unused buffer,
    // unused buffer is swapped with parameter buffer
    // if no new frame: the unused and new buffer will remain the same

    // update the pointer position beforehand because acquirenextframe might return timeout error
    pointer_position = this->pointer_position;
    new_pointer_shape = false;

    CHECK_HR(hr = this->output_duplication->AcquireNextFrame(0, &frame_info, &frame));
    CHECK_HR(hr = frame->QueryInterface(&screen_frame));

    if(!buffer->texture)
    {
        D3D11_TEXTURE2D_DESC screen_frame_desc;

        screen_frame->GetDesc(&screen_frame_desc);
        if(!this->d3d11dev2)
            screen_frame_desc.MiscFlags = 0;
        else
            screen_frame_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        screen_frame_desc.Usage = D3D11_USAGE_DEFAULT;

        CHECK_HR(hr = this->d3d11dev->CreateTexture2D(&screen_frame_desc, NULL, &buffer->texture));
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
        if(!this->d3d11dev2)
            this->d3d11devctx->CopyResource(buffer->texture, screen_frame);
        else
        {
            CComPtr<ID3D11Texture2D> texture_shared;
            CComPtr<IDXGIResource> idxgiresource;
            HANDLE handle;

            // TODO: the texture probably should be opened in d3d10 device
            CHECK_HR(hr = buffer->texture->QueryInterface(&idxgiresource));
            CHECK_HR(hr = idxgiresource->GetSharedHandle(&handle));
            CHECK_HR(hr = this->d3d11dev2->OpenSharedResource(
                handle, __uuidof(ID3D11Texture2D), (void**)&texture_shared));
            this->d3d11devctx2->CopyResource(texture_shared, screen_frame);
        }

        // TODO: the timestamp might not be consecutive
        /*timestamp = clock->performance_counter_to_time_unit(frame_info.LastPresentTime);*/

        // update the newest sample
        std::atomic_exchange(&this->newest_buffer, buffer);
    }
    else
        /*timestamp = clock->get_current_time()*/;

done:
    this->output_duplication->ReleaseFrame();

    if(hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        /*std::cout << "FRAME IS NULL------------------" << std::endl;*/
        /*timestamp = clock->get_current_time();*/
        return false;
    }
    else if(FAILED(hr))
        throw std::exception();

    return (frame_info.LastPresentTime.QuadPart != 0);
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture5::stream_displaycapture5(const source_displaycapture5_t& source, 
    const stream_videoprocessor_controller_t& videoprocessor_controller) : 
    source(source),
    buffer(new media_buffer_texture),
    videoprocessor_controller(videoprocessor_controller),
    last_packet_number(INVALID_PACKET_NUMBER)
{
    this->capture_frame_callback.Attach(new async_callback_t(&stream_displaycapture5::capture_frame_cb));
}

void stream_displaycapture5::capture_frame_cb(void*)
{
    // there exists a possibility for dead lock if another thread tries to read
    // the cached texture, and this thread has already locked the sample;
    // unlocking the capture frame mutex must be ensured before trying to lock the
    // cache texture
    
    std::unique_lock<std::recursive_mutex> lock(this->source->capture_frame_mutex);

    /*const*/ /*media_buffer_texture_t buffer = this_->buffer;*/
        /*this_->source->unused_buffer ? this_->source->unused_buffer : this_->buffer;*/
    /*const*/ /*media_buffer_texture_t pointer_buffer = this_->pointer_stream->buffer;*/
        /*this_->source->unused_pointer_buffer ? 
        this_->source->unused_pointer_buffer : this_->pointer_stream->buffer;*/

    /*assert_(buffer != this->source->newest_buffer);*/

    stream_videoprocessor_controller::params_t params;
    this->videoprocessor_controller->get_params(params);
    // enable alpha is only for pointer which has alpha values
    params.enable_alpha = false;
    media_sample_view_videoprocessor_t sample_view(
        new media_sample_view_videoprocessor(params, this->buffer));
    media_sample_view_videoprocessor_t pointer_sample_view(
        new media_sample_view_videoprocessor(this->pointer_stream->buffer));

    {
        /*media_sample_view_ sample_view2(
            media_sample_texture_(), media_sample_view_::READ_LOCK_BUFFERS);*/
    }


    /*std::unique_lock<std::recursive_mutex> lock(this->source->capture_frame_mutex);*/

    bool frame_captured, new_pointer_shape;
    DXGI_OUTDUPL_POINTER_POSITION pointer_position;
    time_unit timestamp;
    presentation_clock_t clock;

    // capture a frame
    {
        /*scoped_lock lock(this->source->capture_frame_mutex);*/

        // TODO: buffer parameter is redundant because the routine
        // updates the newest buffer field

        // clock is assumed to be valid
        this->rp.get_clock(clock);
        frame_captured = this->source->capture_frame(
            new_pointer_shape, pointer_position, this->pointer_stream->buffer,
            this->buffer, timestamp, clock);
    }

    //if(!frame_captured)
    //{
    //    // TODO: media_sample_view can be allocated in the stack

    //    // sample view must be reset to null before assigning a new sample view,
    //    // that is because the media_sample_view would lock the sample before
    //    // sample_view releasing its own reference to another sample_view
    //    sample_view.reset();
    //    // TODO: do not repeatedly use dynamic allocation
    //    // use the newest buffer from the source;
    //    // the buffer switch must be here so that that sample_view.reset() unlocks the old buffer
    //    sample_view.reset(new media_sample_view_videoprocessor(params,
    //        this_->source->newest_buffer, media_sample_view::READ_LOCK_BUFFERS));

    //    // unused buffer == newest buffer == this buffer

    //    if(this_->source->newest_buffer != this_->buffer)
    //        this_->source->unused_buffer = this_->buffer;
    //    else if(this_->source->unused_buffer == this_->source->newest_buffer)
    //        this_->source->unused_buffer = NULL;
    //}
    //else
    //{
    //    // switch the buffer to read_lock_sample
    //    sample_view->sample.buffer->unlock_write();

    //    if(this_->source->newest_buffer != this_->buffer)
    //        this_->source->unused_buffer = this_->buffer;
    //    else if(this_->source->unused_buffer == this_->source->newest_buffer)
    //        this_->source->unused_buffer = NULL;
    //}

    if(!frame_captured)
    {
        sample_view.reset();
        sample_view.reset(new media_sample_view_videoprocessor(params,
            this->source->newest_buffer, media_sample_view::READ_LOCK_BUFFERS));
    }
    else
        sample_view->sample.buffer->unlock_write();

    if(!new_pointer_shape)
    {
        pointer_sample_view.reset();
        pointer_sample_view.reset(new media_sample_view_videoprocessor(
            this->source->newest_pointer_buffer, media_sample_view::READ_LOCK_BUFFERS));
    }
    else
        pointer_sample_view->sample.buffer->unlock_write();

    /*if(!new_pointer_shape)
    {
        pointer_sample_view.reset();
        pointer_sample_view.reset(new media_sample_view_videoprocessor(
            this_->source->newest_pointer_buffer, media_sample_view::READ_LOCK_BUFFERS));

        if(this_->source->newest_pointer_buffer != this_->pointer_stream->buffer)
            this_->source->unused_pointer_buffer = this_->pointer_stream->buffer;
        else if(this_->source->unused_pointer_buffer == this_->source->newest_pointer_buffer)
            this_->source->unused_pointer_buffer = NULL;
    }
    else
    {
        pointer_sample_view->sample.buffer->unlock_write();

        if(this_->source->newest_pointer_buffer != this_->pointer_stream->buffer)
            this_->source->unused_pointer_buffer = this_->pointer_stream->buffer;
        else if(this_->source->unused_pointer_buffer == this_->source->newest_pointer_buffer)
            this_->source->unused_pointer_buffer = NULL;
    }*/

    request_packet rp = this->rp;
    this->rp = request_packet();

    D3D11_TEXTURE2D_DESC desc;
    D3D11_TEXTURE2D_DESC* ptr_desc = NULL;
    if(sample_view->texture_buffer->texture)
        sample_view->texture_buffer->texture->GetDesc(ptr_desc = &desc);

    sample_view->sample.timestamp = rp.request_time;
    /*this->sample->timestamp = timestamp;*/

    lock.unlock();
    this->process_sample(sample_view, rp, NULL);
    this->pointer_stream->dispatch(
        new_pointer_shape, pointer_position, ptr_desc, pointer_sample_view, rp);
}

media_stream::result_t stream_displaycapture5::request_sample(request_packet& rp, const media_stream*)
{
    if(rp.packet_number == this->last_packet_number)
        return OK;

    this->rp = rp;
    this->last_packet_number = rp.packet_number;

    // dispatch the capture request
    const HRESULT hr = this->capture_frame_callback->mf_put_work_item(
        this->shared_from_this<stream_displaycapture5>());
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
    else if(hr == MF_E_SHUTDOWN)
        return FATAL_ERROR;

    return OK;
}

media_stream::result_t stream_displaycapture5::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    this->source->session->give_sample(this, sample_view, rp, true);
    return OK;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture5_pointer::stream_displaycapture5_pointer(const source_displaycapture5_t& source) :
    source(source),
    buffer(new media_buffer_texture),
    null_buffer(new media_buffer_texture)
{
}

//bool stream_displaycapture5_pointer::set_pointer_rect(
//    media_sample_view_videoprocessor_t& sample_view, 
//    const RECT& src_rect,
//    const RECT& dest_rect) const
//{
//    if(this->desktop_width == -1 || this->desktop_height == -1)
//        return false;
//
//    D3D11_TEXTURE2D_DESC desc;
//    sample_view->texture_buffer->texture->GetDesc(&desc);
//
//    POINT real_pointer_size; // TODO: this is a parameter
//    real_pointer_size.x = desc.Width;
//    real_pointer_size.y = desc.Height;
//
//    // calculate the destination region
//    RECT source_rect_diff;
//    source_rect_diff.left = src_rect.left;
//    source_rect_diff.top = src_rect.top;
//    source_rect_diff.right = this->desktop_width - src_rect.right;
//    source_rect_diff.bottom = this->desktop_height - src_rect.bottom;
//
//    double dest_src_ratio_w, dest_src_ratio_h;
//    dest_src_ratio_w = 
//        (double)(dest_rect.right - dest_rect.left) / (src_rect.right - src_rect.left);
//    dest_src_ratio_h =
//        (double)(dest_rect.bottom - dest_rect.top) / (src_rect.bottom - src_rect.top);
//
//    RECT dest_region = dest_rect;
//    dest_region.left -= (LONG)(source_rect_diff.left * dest_src_ratio_w);
//    dest_region.top -= (LONG)(source_rect_diff.top * dest_src_ratio_h);
//    dest_region.right += (LONG)(source_rect_diff.right * dest_src_ratio_w);
//    dest_region.bottom += (LONG)(source_rect_diff.bottom * dest_src_ratio_h);
//
//    // transform pointer coordinates to destination coordinates
//    POINT pointer_pos = this->pointer_position.Position, pointer_size;
//    pointer_pos.x = (LONG)(pointer_pos.x * dest_src_ratio_w + dest_region.left);
//    pointer_pos.y = (LONG)(pointer_pos.y * dest_src_ratio_h + dest_region.top);
//    pointer_size.x = (LONG)(real_pointer_size.x * dest_src_ratio_w);
//    pointer_size.y = (LONG)(real_pointer_size.y * dest_src_ratio_h);
//
//    // clip pointer to dest rect
//    // (source and dest rect are clipped the same way)
//    // (source rect only has different coordinates)
//    RECT pointer_src_rect, pointer_dst_rect;
//    pointer_dst_rect.left = std::min(std::max(pointer_pos.x, dest_rect.left), dest_rect.right);
//    pointer_dst_rect.right = 
//        std::min(std::max(pointer_pos.x + pointer_size.x, dest_rect.left), dest_rect.right);
//    pointer_dst_rect.top = std::min(std::max(pointer_pos.y, dest_rect.top), dest_rect.bottom);
//    pointer_dst_rect.bottom = 
//        std::min(std::max(pointer_pos.y + pointer_size.y, dest_rect.top), dest_rect.bottom);
//        
//    // TODO: loss of precision
//    pointer_src_rect = pointer_dst_rect;
//    pointer_src_rect.left -= pointer_pos.x;
//    pointer_src_rect.right -= pointer_pos.x;
//    pointer_src_rect.top -= pointer_pos.y;
//    pointer_src_rect.bottom -= pointer_pos.y;
//    pointer_src_rect.left /= dest_src_ratio_w;
//    pointer_src_rect.right /= dest_src_ratio_w;
//    pointer_src_rect.top /= dest_src_ratio_h;
//    pointer_src_rect.bottom /= dest_src_ratio_h;
//    // clamp
//    pointer_src_rect.left = std::max(0L, pointer_src_rect.left);
//    pointer_src_rect.right = std::min(real_pointer_size.x, pointer_src_rect.right);
//    pointer_src_rect.top = std::max(0L, pointer_src_rect.top);
//    pointer_src_rect.bottom = std::min(real_pointer_size.y, pointer_src_rect.bottom);
//
//    /*std::cout << pointer_src_rect.top << ", " << pointer_src_rect.bottom << std::endl;*/
//
//    sample_view->params.source_rect = pointer_src_rect;
//    sample_view->params.dest_rect = pointer_dst_rect;
//    
//    return (pointer_src_rect.left < pointer_src_rect.right &&
//        pointer_src_rect.top < pointer_src_rect.bottom);
//
//    /*const double pointer_ratio_w = 
//        (double)(dest_rect.right - dest_rect.left) / this->desktop_width;
//    const double pointer_ratio_h = 
//        (double)(dest_rect.bottom - dest_rect.top) / this->desktop_height;
//
//    sample_view->params.source_rect.left = sample_view->params.source_rect.top = 0;
//    sample_view->params.source_rect.right = (LONG)desc.Width;
//    sample_view->params.source_rect.bottom = (LONG)desc.Height;
//    sample_view->params.dest_rect.left = (LONG)
//        (dest_rect.left + pointer_position.Position.x * pointer_ratio_w);
//    sample_view->params.dest_rect.right = (LONG)
//        ((dest_rect.left + pointer_position.Position.x * pointer_ratio_w) +
//        desc.Width * pointer_ratio_w);
//    sample_view->params.dest_rect.top = (LONG)
//        (dest_rect.top + pointer_position.Position.y * pointer_ratio_h);
//    sample_view->params.dest_rect.bottom = (LONG)
//        ((dest_rect.top + pointer_position.Position.y * pointer_ratio_h) +
//        desc.Height * pointer_ratio_h);
//
//    const LONG overlap_right_src =
//        std::max(0L, (pointer_position.Position.x + (LONG)desc.Width) - this->desktop_width);
//    const LONG overlap_right_dst =
//        std::max(0L, sample_view->params.dest_rect.right - dest_rect.right);
//    const LONG overlap_bottom_src =
//        std::max(0L, (pointer_position.Position.y + (LONG)desc.Height) - this->desktop_height);
//    const LONG overlap_bottom_dst =
//        std::max(0L, sample_view->params.dest_rect.bottom - dest_rect.bottom);
//    const LONG overlap_left_src = std::min(0L, pointer_position.Position.x);
//    const LONG overlap_left_dst = 
//        std::min(0L, sample_view->params.dest_rect.left - dest_rect.left);
//    const LONG overlap_top_src = std::min(0L, pointer_position.Position.y);
//    const LONG overlap_top_dst =
//        std::min(0L, sample_view->params.dest_rect.top - dest_rect.top);
//
//    sample_view->params.source_rect.right -= overlap_right_src;
//    sample_view->params.dest_rect.right -= overlap_right_dst;
//    sample_view->params.source_rect.bottom -= overlap_bottom_src;
//    sample_view->params.dest_rect.bottom -= overlap_bottom_dst;
//    sample_view->params.source_rect.left -= overlap_left_src;
//    sample_view->params.dest_rect.left -= overlap_left_dst;
//    sample_view->params.source_rect.top -= overlap_top_src;
//    sample_view->params.dest_rect.top -= overlap_top_dst;*/
//
//    /*return true;*/
//}

void stream_displaycapture5_pointer::dispatch(
    bool new_pointer_shape, 
    const DXGI_OUTDUPL_POINTER_POSITION& pointer_position, 
    const D3D11_TEXTURE2D_DESC* desktop_desc,
    media_sample_view_videoprocessor_t& sample_view,
    request_packet& rp)
{
    /*if(!pointer_position.Visible || !desktop_desc)
    {
        sample_view.reset();
        sample_view.reset(new media_sample_view_videoprocessor(
            this->null_buffer, media_sample_view::READ_LOCK_BUFFERS));

        this->source->session->give_sample(this, sample_view, rp, true);
        return;
    }*/

    if(pointer_position.Visible && desktop_desc && sample_view->texture_buffer->texture)
    {
        D3D11_TEXTURE2D_DESC desc;
        sample_view->texture_buffer->texture->GetDesc(&desc);

        sample_view->params.enable_alpha = true;
        sample_view->params.source_rect.left = sample_view->params.source_rect.top = 0;
        sample_view->params.source_rect.right = desc.Width;
        sample_view->params.source_rect.bottom = desc.Height;
        sample_view->params.dest_rect.left = pointer_position.Position.x;
        sample_view->params.dest_rect.top = pointer_position.Position.y;
        sample_view->params.dest_rect.right = sample_view->params.dest_rect.left + desc.Width;
        sample_view->params.dest_rect.bottom = sample_view->params.dest_rect.top + desc.Height;
    }
    else
    {
        sample_view.reset();
        sample_view.reset(new media_sample_view_videoprocessor(
            this->null_buffer, media_sample_view::READ_LOCK_BUFFERS));
    }

    /*if(!new_pointer_shape)
    {
        sample_view.reset();
        sample_view.reset(new media_sample_view_videoprocessor(
            std::atomic_load(&this->source->newest_pointer_buffer), 
            media_sample_view::READ_LOCK_BUFFERS));
    }
    else
    {
        sample_view->sample.buffer->unlock_write();
    }*/

    // set the position where the pointer is blended
    /*if(sample_view->texture_buffer->texture)
    {
        sample_view->params.enable_alpha = true;
        if(!this->set_pointer_rect(sample_view, src_rect, dest_rect))
        {
            sample_view.reset();
            sample_view.reset(new media_sample_view_videoprocessor(
                this->null_buffer, media_sample_view::READ_LOCK_BUFFERS));
        }
    }*/

    sample_view->sample.timestamp = rp.request_time;

    this->source->session->give_sample(this, sample_view, rp, true);
}

media_stream::result_t stream_displaycapture5_pointer::request_sample(request_packet&, const media_stream*)
{
    // do nothing; the process sample is called from the stream displaycapture
    return OK;
}

media_stream::result_t stream_displaycapture5_pointer::process_sample(
    const media_sample_view_t&, request_packet&, const media_stream*)
{
    assert_(false);
    return OK;
}