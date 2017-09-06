#include "source_displaycapture.h"
#include <iostream>
#include <mfapi.h>
#include <cassert>

#pragma comment(lib, "Mfplat.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

extern LARGE_INTEGER pc_frequency;

source_displaycapture::source_displaycapture(const media_session_t& session) : 
    media_source(session)
{
    this->start_time.QuadPart = 0;
}

media_stream_t source_displaycapture::create_stream()
{
    media_stream_t temp;
    temp.Attach(new stream_displaycapture(this->shared_from_this()));
    return temp;
}

HRESULT source_displaycapture::initialize(ID3D11Device* d3d11dev)
{
    scoped_lock lock(this->mutex);

    HRESULT hr;
    CComPtr<IDXGIDevice> dxgidev;
    CComPtr<IDXGIAdapter> dxgiadapter;
    CComPtr<IDXGIOutput> output;
    CComPtr<IDXGIOutput1> output1;

    // get dxgi dev
    CHECK_HR(hr = d3d11dev->QueryInterface(&dxgidev));

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
    CHECK_HR(hr = output1->DuplicateOutput(d3d11dev, &this->output_duplication));

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
        return hr;
    }

    return hr;
}

media_sample_t source_displaycapture::capture_frame(UINT timeout)
{
    scoped_lock lock(this->mutex);
    // TODO: release frame must be implemented in another way
    // TODO: query the current time for the frame if it wasn't updated

    CComPtr<IDXGIResource> frame;
    CComPtr<ID3D11Texture2D> screen_frame;
    media_sample_t sample(new media_sample);

    // release the old frame
    HRESULT hr = this->output_duplication->ReleaseFrame();
    // capture a new frame
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    CHECK_HR(hr = this->output_duplication->AcquireNextFrame(timeout, &frame_info, &frame));
    CHECK_HR(hr = frame->QueryInterface(&screen_frame));

    // create a sample with the new frame

    // time stamp the sample
    LARGE_INTEGER nanosecond_100;
    if(this->start_time.QuadPart == 0)
    {
        nanosecond_100.QuadPart = 0;
        QueryPerformanceCounter(&this->start_time);
    }
    else
    {
        QueryPerformanceCounter(&nanosecond_100);
        nanosecond_100.QuadPart -= this->start_time.QuadPart;
        nanosecond_100.QuadPart *= 1000000 * 10;
        nanosecond_100.QuadPart /= pc_frequency.QuadPart;
    }
    sample->timestamp = nanosecond_100.QuadPart;

done:
    if(FAILED(hr))
        std::cerr << "error occured when trying to acquire next frame" << std::endl;

    return sample;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture::stream_displaycapture(const source_displaycapture_t& source) : 
    source(source), callback(this, &stream_displaycapture::capture_cb)
{
}

HRESULT stream_displaycapture::capture_cb(IMFAsyncResult*)
{
    // TODO: this should signal a fatal error in the topology instead

    media_sample_t sample;
    // request a new frame until a valid frame is returned
    do sample = this->source->capture_frame(INFINITE); while(!sample);

    this->process_sample(sample);
    return S_OK;
}

media_stream::result_t stream_displaycapture::request_sample()
{
    const HRESULT hr = MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &this->callback, NULL);
    if(hr != S_OK)
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_displaycapture::process_sample(const media_sample_t& sample)
{
    this->source->session->give_sample(this, sample, true);
    return OK;
}