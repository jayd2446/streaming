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

extern LARGE_INTEGER pc_frequency;

source_displaycapture5::source_displaycapture5(
    const media_session_t& session, std::recursive_mutex& context_mutex) : 
    media_source(session),
    newest_buffer(new media_buffer_texture),
    context_mutex(context_mutex)
{
}

media_stream_t source_displaycapture5::create_stream()
{
    return media_stream_t(
        new stream_displaycapture5(this->shared_from_this<source_displaycapture5>()));
}

HRESULT source_displaycapture5::initialize(
    UINT output_index, const CComPtr<ID3D11Device>& d3d11dev, const CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr;
    CComPtr<IDXGIDevice> dxgidev;
    CComPtr<IDXGIAdapter> dxgiadapter;
    CComPtr<IDXGIOutput> output;
    CComPtr<IDXGIOutput1> output1;

    this->d3d11dev = d3d11dev;
    this->d3d11devctx = devctx;

    // get dxgi dev
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&dxgidev));

    // get dxgi adapter
    CHECK_HR(hr = dxgidev->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiadapter));

    // get the primary output
    CHECK_HR(hr = dxgiadapter->EnumOutputs(output_index, &output));
    DXGI_OUTPUT_DESC desc;
    CHECK_HR(hr = output->GetDesc(&desc));

    // qi for output1
    CHECK_HR(hr = output->QueryInterface(&output1));

    // create the desktop duplication
    this->output_duplication = NULL;
    CHECK_HR(hr = output1->DuplicateOutput(this->d3d11dev, &this->output_duplication));

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


bool source_displaycapture5::capture_frame(
    const media_buffer_texture_t& buffer, time_unit& timestamp, const presentation_clock_t& clock)
{
    // TODO: context mutex not needed when the dxgioutput is initialized with another device

    // dxgi functions need to be synchronized with the context mutex
    scoped_lock lock(this->context_mutex);

    CComPtr<IDXGIResource> frame;
    CComPtr<ID3D11Texture2D> screen_frame;
    DXGI_OUTDUPL_FRAME_INFO frame_info = {0};
    HRESULT hr = S_OK;

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

    // copy
    if(frame_info.LastPresentTime.QuadPart != 0)
    {
        if(!this->d3d11dev2)
            this->d3d11devctx->CopyResource(buffer->texture, screen_frame);
        else
        {
            CComPtr<ID3D11Texture2D> texture_shared;
            CComPtr<IDXGIResource> idxgiresource;
            CComPtr<ID3D11Resource> resource;
            HANDLE handle;

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
        timestamp = clock->get_current_time();

done:
    this->output_duplication->ReleaseFrame();

    if(hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        /*std::cout << "FRAME IS NULL------------------" << std::endl;*/
        timestamp = clock->get_current_time();
        return false;
    }
    else if(FAILED(hr))
        throw std::exception();

    return (frame_info.LastPresentTime.QuadPart != 0);
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture5::stream_displaycapture5(const source_displaycapture5_t& source) : 
    source(source),
    buffer(new media_buffer_texture)
{
    this->capture_frame_callback.Attach(new async_callback_t(&stream_displaycapture5::capture_frame_cb));
}

void stream_displaycapture5::capture_frame_cb(void*)
{
    // there exists a possibility for dead lock if another thread tries to read
    // the cached texture, and this thread has already locked the sample;
    // unlocking the capture frame mutex must be ensured before trying to lock the
    // cache texture
    media_sample_view_t sample_view(new media_sample_view(this->buffer));
    bool frame_captured;
    time_unit timestamp;
    source_displaycapture5::request_t request;
    presentation_clock_t clock;

    // pull a request and capture a frame for it
    {
        scoped_lock lock(this->source->capture_frame_mutex);

        {
            scoped_lock lock(this->source->requests_mutex);
            request = this->source->requests.front();
            this->source->requests.pop();
        }

        // clock is assumed to be valid
        request.second.get_clock(clock);
        frame_captured = this->source->capture_frame(this->buffer, timestamp, clock);
    }

    // TODO: there exists a chance
    // that the packet which was assigned with request time has greater timestamp
    // than the subsequent packet which is assigned with a valid timestamp
    if(!frame_captured)
    {
        // TODO: media_sample_view can be allocated in the stack

        // sample view must be reset to null before assigning a new sample view,
        // that is because the media_sample_view would lock the sample before
        // sample_view releasing its own reference to another sample_view
        sample_view.reset();
        // TODO: do not repeatedly use dynamic allocation
        // use the newest buffer from the source;
        // the buffer switch must be here so that that sample_view.reset() unlocks the old buffer
        sample_view.reset(new media_sample_view(
            std::atomic_load(&this->source->newest_buffer), media_sample_view::READ_LOCK_BUFFERS));
    }
    else
    {
        // switch the buffer to read_lock_sample
        sample_view->sample.buffer->unlock_write();
    }

    sample_view->sample.timestamp = request.second.request_time;
    /*this->sample->timestamp = timestamp;*/
    request.first->process_sample(sample_view, request.second, NULL);
}

media_stream::result_t stream_displaycapture5::request_sample(request_packet& rp, const media_stream*)
{
    // TODO: decide if displaycapture should capture frames like in source loopback
    // with capture thread priority
    // TODO: requests should be sorted

    {
        scoped_lock lock(this->source->requests_mutex);
        this->source->requests.push(std::make_pair(this, rp));
    }

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