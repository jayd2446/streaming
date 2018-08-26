#include "device_displaycapture.h"
#include <dxgi.h>
#include <mfapi.h>
#include <iostream>
#include <cassert>

#pragma comment(lib, "Mfplat.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

extern LARGE_INTEGER pc_frequency;

class device_displaycapture::capture_callback : IUnknownImpl
{
private:
    AsyncCallback<capture_callback> callback;
    device_displaycapture_t parent;
    MFWORKITEM_KEY workitem_key;
    bool frame_released;
    LARGE_INTEGER start_time;
public:
    explicit capture_callback(const device_displaycapture_t& parent);
    HRESULT schedule_sample_submit();
    HRESULT capture_cb(IMFAsyncResult*);
};

device_displaycapture::capture_callback::capture_callback(const device_displaycapture_t& parent) :
    callback(this, &capture_cb), frame_released(true)
{
    this->start_time.QuadPart = 0;
}

HRESULT device_displaycapture::capture_callback::schedule_sample_submit()
{
    /*if(cancel)
        return MFCancelWorkItem(this->workitem_key);
    else*/
    return MFScheduleWorkItem(&this->callback, NULL, -DISPLAYCAPTURE_RATE_MS, &this->workitem_key);
}

HRESULT device_displaycapture::capture_callback::capture_cb(IMFAsyncResult*)
{
    // TODO: this function must handle multithreading

    // there's no one holding a reference to this device;
    // so delete this so that the circular dependency breaks and the
    // device is destroyed aswell
    if(this->parent->refs == 0)
    {
        delete this;
        return S_OK;
    }

    // schedule a new sample grab
    this->schedule_sample_submit();

    if(!this->frame_released)
    {
        std::cout << "displaycapture dropped a frame due to slow pipeline" << std::endl;
        return S_OK;
    }

    CComPtr<IDXGIResource> frame;
    CComPtr<ID3D11Texture2D> screen_frame;

    // release the old frame
    HRESULT hr = this->parent->output_duplication->ReleaseFrame();
    // capture a new frame
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    CHECK_HR(hr = this->parent->output_duplication->AcquireNextFrame(
        DISPLAYCAPTURE_RATE_MS, &frame_info, &frame));
    this->frame_released = false;
    CHECK_HR(hr = frame->QueryInterface(&screen_frame));

    // update the sample with the new frame

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
    this->parent->sample->timestamp = nanosecond_100.QuadPart;

done:

}


////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////


device_displaycapture::device_displaycapture() : refs(0)
{
}

device_displaycapture::~device_displaycapture()
{
    delete this->callback;
}

HRESULT device_displaycapture::initialize(ID3D11Device* d3d11dev)
{
    this->refs++;
    if(this->refs > 1)
        return S_OK;

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
    if(FAILED(hr))
        return hr;

    // start the display capturing and grab the first frame immediately
    this->callback = new capture_callback(this->shared_from_this());
    this->callback->capture_cb(NULL);
    HRESULT hr2 = this->callback->schedule_sample_submit();
    if(FAILED(hr2))
        delete this->callback;

    return hr2;
}

void device_displaycapture::shutdown()
{
    this->refs--;
    assert(this->refs >= 0);
}