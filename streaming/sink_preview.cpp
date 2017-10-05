#include "sink_preview.h"
//#include "source_displaycapture.h"
//#include "source_displaycapture2.h"
//#include "source_displaycapture3.h"
#include "source_displaycapture4.h"
#include <Mferror.h>
#include <iostream>
#include <mutex>
#include <avrt.h>

#pragma comment(lib, "Avrt.lib")

#define QUEUE_MAX_SIZE 3
#define LAG_BEHIND (FPS60_INTERVAL * 6)
extern LARGE_INTEGER pc_frequency;


sink_preview::sink_preview(const media_session_t& session) : 
    media_sink(session), drawn(false)
{
}

void sink_preview::initialize(
    UINT32 window_width, UINT32 window_height,
    HWND hwnd, 
    CComPtr<ID3D11Device>& d3d11dev, 
    CComPtr<ID3D11DeviceContext>& d3d11devctx,
    CComPtr<IDXGISwapChain>& swapchain)
{
    /*this->displaycapture = d;*/
    this->hwnd = hwnd;
    this->d3d11dev = d3d11dev;
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
    stream_preview_t stream(new stream_preview(this->shared_from_this<sink_preview>()));
    stream->register_sink(clock);
    
    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_preview::stream_preview(const sink_preview_t& sink) : 
    sink(sink), running(false), 
    requests_pending(0)
{
    this->callback.Attach(new async_callback_t(&stream_preview::request_cb));
}

stream_preview::~stream_preview()
{
    /*this->unregister_sink();*/
}

bool stream_preview::on_clock_start(time_unit t)
{
    presentation_clock_t t2;
    this->get_clock(t2);
    this->pipeline_latency = t2->get_current_time();
    this->start_time = t;
    this->running = true;
    this->schedule_new(t);

    request_packet rp;
    rp.request_time = t - LAG_BEHIND;
    return (this->request_sample(rp) != media_stream::FATAL_ERROR);
}

void stream_preview::on_clock_stop(time_unit t)
{
    /*std::cout << "playback stopped" << std::endl;*/
    this->running = false;
    this->clear_queue();
}

DWORD task_index2 = 0;
HANDLE ret = 0;

void stream_preview::scheduled_callback(time_unit due_time)
{
    if(!this->running)
        return;
    
    // schedule a new time
    this->schedule_new(due_time);

    const HRESULT hr = this->callback->mf_put_work_item(
        this->shared_from_this<stream_preview>(), MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
    /*}
    else
        std::cout << "--SAMPLE REQUEST DROPPED IN STREAM_PREVIEW--" << std::endl;*/
    /*if(this->request_sample(due_time) == FATAL_ERROR)
        this->running = false;*/
}

bool bb = true;

void stream_preview::schedule_new(time_unit due_time)
{
    presentation_clock_t t;
    this->get_clock(t);
    if(t)
    {
        if(!bb)
            return;

        // 60 fps
        static int counter = 0;
        /*static time_unit last_scheduled_time = 0;*/
        time_unit pull_interval = 166667;
        counter++;
        /*if((counter % 3) == 0)
            pull_interval -= 1;*/

        // x = scheduled_time, 17 = pull_interval
        // (x+17) - floor(((3 * (x+17)) mod 50) / 3), x from 0 to 1
        const time_unit current_time = t->get_current_time();
        time_unit scheduled_time = due_time;

        scheduled_time += pull_interval;
        scheduled_time -= ((3 * scheduled_time) % 500000) / 3;

        /*std::cout << numbers << ". ";*/

        /*last_scheduled_time = scheduled_time;*/
        if(!this->schedule_new_callback(scheduled_time))
        {
            if(scheduled_time > current_time)
            {
                // the scheduled time is so close to current time that the callback cannot be set
                std::cout << "VERY CLOSE" << std::endl;
                /*std::cout << sample->timestamp << std::endl;*/
                {
                    std::lock_guard<std::recursive_mutex> lock(this->mutex);
                    this->requests.push(scheduled_time);
                }
                this->scheduled_callback(scheduled_time);
            }
            else
            {
                //// at least one frame was late
                //std::cout << "--------------------------------------------------------------------------------------------" << std::endl;
                
                // TODO: calculate here how many frame requests missed
                do
                {
                    // this commented line will skip the loop and calculate the
                    // next frame
                    /*const time_unit current_time2 = t->get_current_time();
                    scheduled_time = current_time2;*/

                    // frame request was late
                    std::cout << "--------------------------------------------------------------------------------------------" << std::endl;

                    scheduled_time += pull_interval;
                    scheduled_time -= ((3 * scheduled_time) % 500000) / 3;
                }
                while(!this->schedule_new_callback(scheduled_time));
            }
        }
        else
            /*std::cout << scheduled_time << std::endl*/;

        std::lock_guard<std::recursive_mutex> lock(this->mutex);
        this->requests.push(scheduled_time);
    }
}

void stream_preview::request_cb()
{
    time_unit request_time;
    request_packet rp;
    {
        std::lock_guard<std::recursive_mutex> lock(this->mutex);
        request_time = this->requests.front() - LAG_BEHIND;
        this->requests.pop();
    }
    
    // TODO: there's still a chance that the requests queue will over saturate
    // which implies a very massive lag
    if(this->requests_pending >= QUEUE_MAX_SIZE)
    {
        std::cout << "--SAMPLE REQUEST DROPPED IN STREAM_PREVIEW--" << std::endl;
        return;
    }

    this->requests_pending++;
    rp.request_time = request_time;
    if(this->request_sample(rp) == FATAL_ERROR)
    {
        this->requests_pending--;
        this->running = false;
    }
}

bool stream_preview::get_clock(presentation_clock_t& clock)
{
    return this->sink->session->get_current_clock(clock);
}

media_stream::result_t stream_preview::request_sample(request_packet& rp)
{
    if(!this->sink->session->request_sample(this, rp, true))
        return FATAL_ERROR;

    //presentation_clock_t t;
    //if(this->get_clock(t))
    //    /*for(int i = 0; i < 10; i++)*/
    //        this->schedule_new_callback(t->get_current_time() + std::rand() % 110000 + 10000);

    /*std::cout << "NEW" << std::endl;*/

    return OK;
}

media_stream::result_t stream_preview::process_sample(
    const media_sample_t& sample, request_packet& rp)
{
    // schedule the sample
    // 5000000 = half a second

    // render the sample to the backbuffer here

    // TODO: drawing etc should be put to a work queue

    // TODO: scheduling can be removed from the sink and replaced with
    // a request each time a new sample arrives(that might hinder the concurrency a bit)

    static HANDLE last_frame;
    static time_unit last_request_time = 0;
    HRESULT hr = S_OK;
    if(sample->frame)
    {
        // TODO: decide if the mutex is necessary here
        std::lock_guard<std::mutex> lock(this->render_mutex);
        this->sink->drawn = true;
        CComPtr<IDXGISurface> surface;
        hr = this->sink->d3d11dev->OpenSharedResource(
            sample->frame, __uuidof(IDXGISurface), (void**)&surface);
        CComPtr<ID2D1Bitmap1> frame;
        CComPtr<IDXGIKeyedMutex> frame_mutex;
        hr = surface->QueryInterface(&frame_mutex);
        frame_mutex->AcquireSync(1, INFINITE);
        hr = this->sink->d2d1devctx->CreateBitmapFromDxgiSurface(
            surface,
            D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
            &frame);

        // 10000000

        this->sink->d2d1devctx->BeginDraw();
        this->sink->d2d1devctx->DrawBitmap(frame);
        hr = this->sink->d2d1devctx->EndDraw();
        frame_mutex->ReleaseSync(1);

        /*this->sink->drawn = false;*/
    }
    else
        this->sink->drawn = false;
    
    if(last_request_time > rp.request_time)
    {
        std::cout << "OUT OF ORDER FRAME" << std::endl;
    }

    last_frame = sample->frame;
    last_request_time = rp.request_time;

    // unlock the frame
    sample->unlock_sample();

    // calculate fps
    // (fps under 60 means that the frames are coming out of order
    // or that the pipeline is becoming increasingly saturated)
    static int numbers = 0;
    numbers++;
    /*if(numbers == 60)
    {
        std::cout << "last frame time: " << request_time << std::endl;
        numbers = 0;
    }*/
    if((rp.request_time % 10000000) == 0)
    {
        std::cout << numbers << std::endl;
        numbers = 0;
    }

    if(this->sink->drawn)
        this->sink->swapchain->Present(0, 0);

    this->requests_pending--;

    /*std::cout << "frame time: " << request_time << std::endl;*/

    /*if(this->sink->displaycapture->new_available)
    {
        this->sink->displaycapture->give_back_texture();
        this->sink->displaycapture->new_available = false;
    }*/
    /*this->sink->displaycapture->output_duplication->ReleaseFrame();*/

    // TODO: keep track of latest request time


    // SCHEDULING A NEW TIME
    //presentation_clock_t t;
    //this->get_clock(t);
    //if(t)
    //{
    //    // 60 fps
    //    static int counter = 0;
    //    static int numbers = 0;
    //    static time_unit last_scheduled_time = 0;
    //    time_unit pull_interval = 166667;
    //    counter++;
    //    /*if((counter % 3) == 0)
    //        pull_interval -= 1;*/

    //    // x = scheduled_time, 17 = pull_interval
    //    // (x+17) - floor(((3 * (x+17)) mod 50) / 3), x from 0 to 1
    //    const time_unit current_time = t->get_current_time();
    //    time_unit scheduled_time = max(sample->timestamp, last_scheduled_time);

    //    /*scheduled_time += 500000;
    //    scheduled_time -= scheduled_time % 500000;
    //    if(sample->timestamp < (scheduled_time - 166667 - 166666))
    //        scheduled_time = scheduled_time - 166667 - 166666;
    //    else if(sample->timestamp < (scheduled_time - 166666))
    //        scheduled_time = scheduled_time - 166666;*/
    //    scheduled_time += pull_interval;
    //    // scheduled_time % pull_interval
    //    scheduled_time -= ((3 * scheduled_time) % 500000) / 3;

    //    numbers++;
    //    if((scheduled_time % 10000000) == 0)
    //    {
    //        std::cout << numbers << std::endl;
    //        numbers = 0;
    //    }

    //    /*std::cout << numbers << ". ";*/

    //    last_scheduled_time = scheduled_time;
    //    if(!this->schedule_new_callback(scheduled_time))
    //    {
    //        if(scheduled_time > current_time)
    //        {
    //            // the scheduled time is so close to current time that the callback cannot be set
    //            std::cout << "VERY CLOSE" << std::endl;
    //            /*std::cout << sample->timestamp << std::endl;*/
    //            this->scheduled_callback(scheduled_time);
    //        }
    //        else
    //        {
    //            // frame was late
    //            std::cout << "--------------------------------------------------------------------------------------------" << std::endl;
    //            /*this->sink->drawn = false;*/

    //            
    //            do
    //            {
    //                const time_unit current_time2 = t->get_current_time();
    //                scheduled_time = current_time2;

    //                /*scheduled_time += 500000;
    //                scheduled_time -= scheduled_time % 500000;
    //                if(current_time2 < (scheduled_time - 166667 - 166666))
    //                    scheduled_time = scheduled_time - 166667 - 166666;
    //                else if(current_time2 < (scheduled_time - 166666))
    //                    scheduled_time = scheduled_time - 166666;*/
    //                scheduled_time += pull_interval;
    //                // scheduled_time % pull_interval
    //                scheduled_time -= ((3 * scheduled_time) % 500000) / 3;

    //                last_scheduled_time = scheduled_time;
    //            }
    //            while(!this->schedule_new_callback(scheduled_time));
    //        }
    //    }
    //    else
    //        /*std::cout << scheduled_time << std::endl*/;
    //}

    return OK;
}