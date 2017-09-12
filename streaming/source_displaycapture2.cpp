#include "source_displaycapture2.h"
#include <iostream>
#include <mfapi.h>
#include <mutex>
#include <avrt.h>

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Avrt.lib")
#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
extern LARGE_INTEGER pc_frequency;

source_displaycapture2::source_displaycapture2(const media_session_t& session) : 
    media_source(session),
    active_frame(0),
    buffered_frame(1),
    new_available(false)
{
    this->start_time.QuadPart = 0;
}

media_stream_t source_displaycapture2::create_stream(presentation_clock_t& clock)
{
    media_stream_t temp;
    temp.Attach(new stream_displaycapture2(
        std::dynamic_pointer_cast<source_displaycapture2>(this->shared_from_this()), clock));
    return temp;
}

HRESULT source_displaycapture2::initialize(
    CComPtr<ID3D11Device>& d3d11dev,
    CComPtr<ID3D11DeviceContext>& devctx)
{
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

    this->d3d11devctx = devctx;
    this->d3d11 = d3d11dev;

    return hr;
}

extern std::recursive_mutex mutex_;

void source_displaycapture2::capture_frame()
{
    CComPtr<IDXGIResource> frame;
    CComPtr<ID3D11Texture2D> screen_frame;
    CComPtr<IDXGISurface> surface;
    DXGI_OUTDUPL_FRAME_INFO frame_info = {0};

    HRESULT hr;
    hr = this->output_duplication->ReleaseFrame();
    CHECK_HR(this->output_duplication->AcquireNextFrame(INFINITE, &frame_info, &frame));
   /* std::cout << frame_info.LastPresentTime.QuadPart << std::endl;*/

    CHECK_HR(hr = frame->QueryInterface(&screen_frame));

    // TODO: add mutexes for the buffered frames

    // buffer the screen frame
    if(!this->screen_frame[this->active_frame])
    {
        // allocate buffer
        D3D11_TEXTURE2D_DESC screen_frame_desc;
        screen_frame->GetDesc(&screen_frame_desc);
        screen_frame_desc.MiscFlags = 0;
        CHECK_HR(hr = this->d3d11->CreateTexture2D(
            &screen_frame_desc, 
            NULL, 
            &this->screen_frame[this->active_frame]));
    }

    mutex_.lock();
    this->d3d11devctx->CopyResource(this->screen_frame[this->active_frame], screen_frame);
    mutex_.unlock();
    this->new_available = true;
    /*this->active_frame = (this->active_frame + 1) % 2;*/

done:
    if(FAILED(hr))
        throw std::exception();
}

CComPtr<ID3D11Texture2D> source_displaycapture2::give_texture()
{
    CComPtr<ID3D11Texture2D> ret(this->screen_frame[this->active_frame]);
    if(this->new_available)
        this->active_frame = (this->active_frame + 1) % 2;
    return ret;
}

void source_displaycapture2::give_back_texture()
{
    /*if(this->active_frame == this->buffered_frame)*/
        /*this->buffered_frame = (this->buffered_frame + 1) % 2;*/
        /*this->buffered_frame = this->active_frame;
        this->active_frame = (this->active_frame + 1) % 2;*/
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture2::stream_displaycapture2(
    const source_displaycapture2_t& source, presentation_clock_t& clock) : 
    source(source), 
    callback(this, &stream_displaycapture2::capture_cb),
    running(false),
    presentation_clock_sink(clock)
{
    if(FAILED(MFAllocateWorkQueue(&this->work_queue)))
        throw std::exception();
}

stream_displaycapture2::~stream_displaycapture2()
{
    MFUnlockWorkQueue(this->work_queue);
}

bool stream_displaycapture2::on_clock_start(time_unit t)
{
    this->running = true;
    const HRESULT hr = MFPutWorkItem(this->work_queue, &this->callback, NULL);
    if(hr != S_OK)
        return false;
    return true;
}

void stream_displaycapture2::on_clock_stop(time_unit t)
{
    this->running = false;
}

void stream_displaycapture2::scheduled_callback(time_unit)
{
    /*const time_unit current_time = clock->get_current_time();*/
    const time_unit pull_interval = 166666; // ~60 fps

    // capture the newest frame
    /*time_unit current_time = clock->get_current_time();
    time_unit scheduled_time = current_time;
    scheduled_time += pull_interval;
    scheduled_time -= scheduled_time % pull_interval;*/


    // set time for the next callback
    time_unit scheduled_time, current_time;
    {
        presentation_clock_t clock;
        if(!this->get_clock(clock))
        {
            this->running = false;
            return;
        }
        current_time = clock->get_current_time();
        scheduled_time = current_time;
        scheduled_time += pull_interval;
        scheduled_time -= scheduled_time % pull_interval;
    }

    // capture the frame
    this->source->capture_frame();

    // schedule a new callback
    presentation_clock_t clock;
    if(!this->get_clock(clock))
    {
        this->running = false;
        return;
    }
    current_time = clock->get_current_time();
    if(!this->schedule_new_callback(scheduled_time))
    {
        if(scheduled_time > current_time)
        {
            // the scheduled time is so close to current time that the callback cannot be set
            this->scheduled_callback(scheduled_time);
        }
        else
        {
            // frame was late
            std::cout << "FRAME WAS LATE" << std::endl;
            scheduled_time = clock->get_current_time();
            do
            {
                scheduled_time += pull_interval;
                scheduled_time -= scheduled_time % pull_interval;
            }
            while(!this->schedule_new_callback(scheduled_time));
        }
    }
    //while(frame_was_late = !this->schedule_new_callback(scheduled_time));


    //// schedule a new callback
    //if(this->running)
    //{
    //    const time_unit current_time = clock->get_current_time();
    //    time_unit scheduled_time = current_time;
    //    scheduled_time += pull_interval;
    //    scheduled_time -= scheduled_time % pull_interval;

    //    if(!this->schedule_new_callback(scheduled_time))
    //    {
    //        //if(scheduled_time <= current_time)
    //        //{
    //        //    // the scheduled time is so close to current time that the callback cannot be set
    //        //    // TODO: check this
    //        //    this->scheduled_callback(scheduled_time);
    //        //}
    //        //else
    //        //{
    //        //    // the fetched frame was late
    //        //    std::cout << "FRAME IS NULL------------------" << std::endl;

    //        //}
    //    }
    //}
}

HRESULT stream_displaycapture2::capture_cb(IMFAsyncResult*)
{
    /*DWORD task_index = 0;
    HANDLE h;
    if(!task_index)
        h = AvSetMmThreadCharacteristics(L"Capture", &task_index);
    else
        h = AvSetMmThreadCharacteristics(L"Capture", &task_index);
    if(!h)
    {
        DWORD d = GetLastError();
        throw std::exception();
    }*/

    presentation_clock_t clock;
    if(this->get_clock(clock) && this->running)
    {
        // start the periodic screen frame fetching
        this->running = true;
        this->source->capture_frame();
        /*this->scheduled_callback(clock->get_current_time());*/
    }


    /*AvRevertMmThreadCharacteristics(h);*/

    return S_OK;
}

media_stream::result_t stream_displaycapture2::request_sample()
{
    // return the oldest buffered sample and add the timestamp
    CComPtr<ID3D11Texture2D> screen_frame = this->source->give_texture();
    presentation_clock_t clock;
    if(!this->get_clock(clock))
        return FATAL_ERROR;

    media_sample_t sample(new media_sample);
    sample->frame = screen_frame;
    sample->timestamp = clock->get_current_time();

    // add new request
    if(this->source->new_available)
    {
        this->source->new_available = false;
        HRESULT hr = MFPutWorkItem(this->work_queue, &this->callback, NULL);
    }
    else
        std::cout << "FRAME IS NULL------------------" << std::endl;

    this->process_sample(sample);
    return OK;
}

media_stream::result_t stream_displaycapture2::process_sample(const media_sample_t& sample)
{
    // pass the sample to downstream
    this->source->session->give_sample(this, sample, true);
    return OK;
}