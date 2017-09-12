#include "sink_preview.h"
#include "source_displaycapture.h"
#include "source_displaycapture2.h"
#include <iostream>
#include <mutex>
#include <avrt.h>

#pragma comment(lib, "Avrt.lib")

extern LARGE_INTEGER pc_frequency;

std::recursive_mutex mutex_;

sink_preview::sink_preview(const media_session_t& session) : 
    media_sink(session), drawn(false)
{
}

void sink_preview::initialize(
    source_displaycapture2* displaycapture,
    UINT32 window_width, UINT32 window_height,
    HWND hwnd, 
    CComPtr<ID3D11Device>& d3d11dev, 
    CComPtr<ID3D11DeviceContext>& d3d11devctx,
    CComPtr<IDXGISwapChain>& swapchain)
{
    /*this->displaycapture = d;*/
    this->hwnd = hwnd;
    this->d3d11dev = d3d11dev;
    this->displaycapture = displaycapture;
    /*this->d3d11dev = d3d11dev;
    this->d3d11devctx = d3d11devctx;
    this->swapchain = swapchain;*/

    // create d2d factory
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &this->d2d1factory);

    // obtain the dxgi device of the d3d11 device
    hr = this->d3d11dev->QueryInterface(&this->dxgidev);

    // obtain the direct2d device
    hr = this->d2d1factory->CreateDevice(this->dxgidev, &this->d2d1dev);

    // get the direct2d device context
    hr = this->d2d1dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &this->d2d1devctx);

    // create swap chain for the hwnd
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {0};
    swapchain_desc.Width = swapchain_desc.Height = 0; // use automatic sizing
    // this is the most common swapchain
    swapchain_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapchain_desc.Stereo = false;
    swapchain_desc.SampleDesc.Count = 1; // dont use multisampling
    swapchain_desc.SampleDesc.Quality = 0;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = 2; // use double buffering to enable flip
    swapchain_desc.Scaling = DXGI_SCALING_NONE;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapchain_desc.Flags = 0;

    // identify the physical adapter (gpu or card) this device runs on
    CComPtr<IDXGIAdapter> dxgiadapter;
    hr = this->dxgidev->GetAdapter(&dxgiadapter);
    hr = dxgiadapter->EnumOutputs(0, &this->dxgioutput);

    // get the factory object that created this dxgi device
    CComPtr<IDXGIFactory2> dxgifactory;
    hr = dxgiadapter->GetParent(IID_PPV_ARGS(&dxgifactory));

    // get the final swap chain for this window from the dxgi factory
    hr = dxgifactory->CreateSwapChainForHwnd(
        this->d3d11dev, hwnd, &swapchain_desc, NULL, NULL, &this->swapchain);

    // ensure that dxgi doesn't queue more than one frame at a time
    /*hr = this->dxgidev->SetMaximumFrameLatency(1);*/

    // get the backbuffer for this window which is the final 3d render target
    CComPtr<ID3D11Texture2D> backbuffer;
    hr = this->swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));

    // now we set up the direct2d render target bitmap linked to the swapchain
    // whenever we render to this bitmap, it is directly rendered to the swap chain associated
    // with this window
    D2D1_BITMAP_PROPERTIES1 bitmap_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    // direct2d needs the dxgi version of the backbuffer surface pointer
    CComPtr<IDXGISurface> dxgibackbuffer;
    hr = this->swapchain->GetBuffer(0, IID_PPV_ARGS(&dxgibackbuffer));

    // get the d2d surface from the dxgi back buffer to use as the d2d render target
    hr = this->d2d1devctx->CreateBitmapFromDxgiSurface(
        dxgibackbuffer, &bitmap_props, &this->d2dtarget_bitmap);

    // now we can set the direct2d render target
    this->d2d1devctx->SetTarget(this->d2dtarget_bitmap);
}

media_stream_t sink_preview::create_stream(presentation_clock_t& clock)
{
    media_stream_t temp;
    temp.Attach(new stream_preview(
        std::dynamic_pointer_cast<sink_preview>(this->shared_from_this()), clock));
    return temp;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_preview::stream_preview(const sink_preview_t& sink, presentation_clock_t& clock) : 
    sink(sink), presentation_clock_sink(clock)
{
}

stream_preview::~stream_preview()
{
    /*std::cout << "DESTRUCTOR" << std::endl;*/
    int i = 0;
    i++;
}

bool stream_preview::on_clock_start(time_unit t)
{
    presentation_clock_t t2;
    this->get_clock(t2);
    this->pipeline_latency = t2->get_current_time();
    this->start_time = t;
    return (this->request_sample() != media_stream::FATAL_ERROR);
    /*this->scheduled_callback(t);*/
    return true;
}

void stream_preview::on_clock_stop(time_unit t)
{
    /*std::cout << "playback stopped" << std::endl;*/
    if(!this->clear_queue())
        /*std::cout << "MFCANCELWORKITEM FAILED" << std::endl*/;
}

DWORD task_index2 = 0;

void stream_preview::scheduled_callback(time_unit due_time)
{
    /*this->sink->dxgioutput->WaitForVBlank();*/
    //HANDLE ret;
    //if(!task_index2)
    //    ret = AvSetMmThreadCharacteristics(L"Playback", &task_index2);
    //else
    //    ret = AvSetMmThreadCharacteristics(L"Playback", &task_index2);
    //if(!ret)
    //{
    //    /*DWORD d = GetLastError();*/
    //    throw std::exception();
    //}
    
    if(this->sink->drawn)
        this->sink->swapchain->Present(0, 0);
    /*std::cout << "sample shown @ " << due_time << std::endl;*/
    this->request_sample();
}

bool stream_preview::get_clock(presentation_clock_t& clock)
{
    return this->sink->session->get_current_clock(clock);
}

media_stream::result_t stream_preview::request_sample()
{
    if(!this->sink->session->request_sample(this, true))
        return FATAL_ERROR;

    //presentation_clock_t t;
    //if(this->get_clock(t))
    //    /*for(int i = 0; i < 10; i++)*/
    //        this->schedule_new_callback(t->get_current_time() + std::rand() % 110000 + 10000);

    /*std::cout << "NEW" << std::endl;*/

    return OK;
}

media_stream::result_t stream_preview::process_sample(const media_sample_t& sample)
{
    // schedule the sample
    // 5000000 = half a second

    // render the sample to the backbuffer here

    /*
    drawing = saving
    presenting = loading
    */

    /*
    displaycapture2:
    on request, give the buffered sample and begin acquiring a new sample;
    do not queue a new acquire if new request happens while acquiring
    */

    if(sample->frame)
    {
        this->sink->drawn = true;
        CComPtr<IDXGISurface> surface;
        HRESULT hr = sample->frame->QueryInterface(&surface);
        CComPtr<ID2D1Bitmap1> frame;
        mutex_.lock();
        hr = this->sink->d2d1devctx->CreateBitmapFromDxgiSurface(
            surface,
            D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
            &frame);

        this->sink->d2d1devctx->BeginDraw();
        this->sink->d2d1devctx->DrawBitmap(frame);
        hr = this->sink->d2d1devctx->EndDraw();
        mutex_.unlock();
    }
    else
        this->sink->drawn = false;
    /*if(this->sink->displaycapture->new_available)
    {
        this->sink->displaycapture->give_back_texture();
        this->sink->displaycapture->new_available = false;
    }*/
    /*this->sink->displaycapture->output_duplication->ReleaseFrame();*/

    presentation_clock_t t;
    this->get_clock(t);
    if(t)
    {
        const time_unit pull_interval = 166666; // ~60 fps
        const time_unit current_time = t->get_current_time();
        time_unit scheduled_time = sample->timestamp;
        scheduled_time += pull_interval;
        scheduled_time -= scheduled_time % pull_interval;

        if(!this->schedule_new_callback(scheduled_time))
        {
            if(scheduled_time > current_time)
            {
                const time_unit current_time2 = t->get_current_time();
                // the scheduled time is so close to current time that the callback cannot be set
                std::cout << "VERY CLOSE" << std::endl;
                /*std::cout << sample->timestamp << std::endl;*/
                this->scheduled_callback(scheduled_time);
            }
            else
            {
                // frame was late
                std::cout << "--------------------------------------------------------------------------------------------" << std::endl;
                /*this->sink->drawn = false;*/

                scheduled_time = t->get_current_time();
                do
                {
                    scheduled_time += pull_interval;
                    scheduled_time -= scheduled_time % pull_interval;
                }
                while(!this->schedule_new_callback(scheduled_time));
                
            }
        }
        else
            std::cout << sample->timestamp << std::endl;
    }

    return OK;
}