#include "source_displaycapture3.h"
#include <iostream>
#include <mfapi.h>

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "D3D11.lib")
#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
extern LARGE_INTEGER pc_frequency;

source_displaycapture3::source_displaycapture3(const media_session_t& session) : 
    media_source(session), screen_frame_handle(NULL)
{
}

media_stream_t source_displaycapture3::create_stream()
{
    media_stream_t temp;
    temp.Attach(new stream_displaycapture3(
        std::dynamic_pointer_cast<source_displaycapture3>(this->shared_from_this())));
    return temp;
}

HRESULT source_displaycapture3::initialize(
    CComPtr<ID3D11Device>& d3d11dev,
    CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr;
    CComPtr<IDXGIDevice> dxgidev;
    CComPtr<IDXGIAdapter> dxgiadapter;
    CComPtr<IDXGIOutput> output;
    CComPtr<IDXGIOutput1> output1;

    // create d3d11 device
    hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION, &this->d3d11dev, NULL, &this->d3d11devctx);

    // get dxgi dev
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&dxgidev));

    // get dxgi adapter
    CHECK_HR(hr = dxgidev->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiadapter));

    // get the primary output
    CHECK_HR(hr = dxgiadapter->EnumOutputs(0, &output));
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

HANDLE source_displaycapture3::capture_frame()
{
    CComPtr<IDXGIResource> frame;
    CComPtr<ID3D11Texture2D> screen_frame;
    CComPtr<IDXGIKeyedMutex> frame_mutex;
    DXGI_OUTDUPL_FRAME_INFO frame_info = {0};

    HRESULT hr = S_OK;
    CHECK_HR(hr = this->output_duplication->AcquireNextFrame(0, &frame_info, &frame));
    CHECK_HR(hr = frame->QueryInterface(&screen_frame));

    UINT64 key = 1;
    if(!this->screen_frame)
    {
        // allocate buffer
        D3D11_TEXTURE2D_DESC screen_frame_desc;
        screen_frame->GetDesc(&screen_frame_desc);
        screen_frame_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        CHECK_HR(hr = this->d3d11dev->CreateTexture2D(&screen_frame_desc, NULL, &this->screen_frame));

        CComPtr<IDXGIResource> idxgiresource;
        CHECK_HR(hr = this->screen_frame->QueryInterface(&idxgiresource));
        CHECK_HR(hr = idxgiresource->GetSharedHandle(&this->screen_frame_handle));
        key = 0;
    }

    CHECK_HR(hr = this->screen_frame->QueryInterface(&frame_mutex));
    CHECK_HR(hr = frame_mutex->AcquireSync(key, INFINITE));
    this->d3d11devctx->CopyResource(this->screen_frame, screen_frame);
    if(key == 0)
        key = 1;
    CHECK_HR(hr = frame_mutex->ReleaseSync(key));

done:
    this->output_duplication->ReleaseFrame();

    if(hr == DXGI_ERROR_WAIT_TIMEOUT)
        std::cout << "FRAME IS NULL------------------" << std::endl;
    else if(FAILED(hr))
        throw std::exception();

    return this->screen_frame_handle;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture3::stream_displaycapture3(const source_displaycapture3_t& source) : 
    source(source), 
    callback(this, &stream_displaycapture3::capture_cb)
{
}

HRESULT stream_displaycapture3::capture_cb(IMFAsyncResult*)
{
    media_sample_t sample(new media_sample);
    sample->frame = this->source->capture_frame();
    this->process_sample(sample);
    
    return S_OK;
}

media_stream::result_t stream_displaycapture3::request_sample()
{
    const HRESULT hr = MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &this->callback, NULL);
    if(hr != S_OK)
        return FATAL_ERROR;
    /*media_sample_t sample(new media_sample);
    sample->frame = NULL;
    this->process_sample(sample);*/

    return OK;
}

media_stream::result_t stream_displaycapture3::process_sample(const media_sample_t& sample)
{
    // add the timestamp
    presentation_clock_t clock;
    if(!this->get_clock(clock))
        return FATAL_ERROR;

    sample->timestamp = clock->get_current_time();

    // pass the sample to downstream
    this->source->session->give_sample(this, sample, true);
    return OK;
}