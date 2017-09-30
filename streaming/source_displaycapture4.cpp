#include "source_displaycapture4.h"
#include <iostream>
#include <mfapi.h>
#include <Mferror.h>
#include <cassert>

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "D3D11.lib")
#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
extern LARGE_INTEGER pc_frequency;

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#define MONITOR_INDEX 0
#define MAX_DIFF std::numeric_limits<time_unit>::max()/*(FPS60_INTERVAL / 2 + FPS60_INTERVAL / 4)*/
#define LAG_BEHIND (FPS60_INTERVAL * 3)

source_displaycapture4::thread_capture::thread_capture(source_displaycapture4_t& source,
    presentation_clock_t& clock) : 
    source(source),
    presentation_clock_sink(clock),
    running(false),
    callback(this, &source_displaycapture4::thread_capture::capture_cb)
{
    if(FAILED(MFAllocateWorkQueue(&this->work_queue)))
        throw std::exception();
}

source_displaycapture4::thread_capture::~thread_capture()
{
    MFUnlockWorkQueue(this->work_queue);
}

bool source_displaycapture4::thread_capture::on_clock_start(time_unit t)
{
    if(this->running)
        return true;

    this->running = true;
    this->schedule_new(t);
    return true;
}

void source_displaycapture4::thread_capture::on_clock_stop(time_unit t)
{
    if(!this->running)
        return;

    this->running = false;
}

bool source_displaycapture4::thread_capture::get_clock(presentation_clock_t& clock)
{
    source_displaycapture4_t source;
    if(this->get_source(source))
    {
        clock = source->get_device_clock();
        return !!clock;
    }
    else
        return false;
}

void source_displaycapture4::thread_capture::scheduled_callback(time_unit due_time)
{
    this->due_time = due_time;

    const HRESULT hr = MFPutWorkItem(this->work_queue, &this->callback, NULL);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
}

HRESULT source_displaycapture4::thread_capture::capture_cb(IMFAsyncResult*)
{
    source_displaycapture4_t source;

    if(!this->running || !this->get_source(source))
        return S_OK;

    // do not schedule a new time until the old capture has succeeded,
    // because the capturing might exceed the next scheduled time;
    // do not schedule a new time until the frame that is going to be modified has become
    // available aswell
    presentation_clock_t clock;
    if(this->get_clock(clock))
    {
        scoped_lock frame_lock(source->samples[source->current_frame]->mutex);
        source->capture_frame(clock->get_start_time());
        this->schedule_new(this->due_time);
    }
    else
        this->on_clock_stop(0);
    return S_OK;
}

bool source_displaycapture4::thread_capture::get_source(source_displaycapture4_t& source)
{
    source = this->source.lock();
    return !!source;
}

void source_displaycapture4::thread_capture::schedule_new(time_unit due_time)
{
    presentation_clock_t t;
    if(this->get_clock(t))
    {
        const time_unit pull_interval = 166667;
        const time_unit current_time = t->get_current_time();
        time_unit scheduled_time = due_time;

        scheduled_time += pull_interval;
        scheduled_time -= ((3 * scheduled_time) % 500000) / 3;

        if(!this->schedule_new_callback(scheduled_time))
        {
            if(scheduled_time > current_time)
            {
                // the scheduled time is so close to current time that the callback cannot be set
                std::cout << "VERY CLOSE" << std::endl;
                this->scheduled_callback(scheduled_time);
            }
            else
            {
                // TODO: calculate here how many frame requests missed
                do
                {
                    // this commented line will skip the loop and calculate the
                    // next frame
                    /*const time_unit current_time2 = t->get_current_time();
                    scheduled_time = current_time2;*/

                    // frame request was late
                    std::cout << "--FRAME DROPPED--" << std::endl;
                    /*std::cout << "--FRAME CAPTURE THREAD WAS LATE--" << std::endl;*/

                    scheduled_time += pull_interval;
                    scheduled_time -= ((3 * scheduled_time) % 500000) / 3;
                }
                while(!this->schedule_new_callback(scheduled_time));
            }
        }
    }
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


source_displaycapture4::source_displaycapture4(const media_session_t& session) : 
    media_source(session), current_frame(0)
{
    for(size_t i = 0; i < SAMPLE_DEPTH; i++)
    {
        this->samples[i].reset(new media_sample);
        this->samples[i]->frame = NULL;
        this->samples[i]->timestamp = 0;
    }
}

source_displaycapture4::~source_displaycapture4()
{
    scoped_lock lock(this->requests_mutex);
    while(!this->requests.empty())
    {
        this->requests.front().cb->Release();
        this->requests.pop_front();
    }
}

media_stream_t source_displaycapture4::create_stream()
{
    media_stream_t temp;
    temp.Attach(new stream_displaycapture4(
        std::dynamic_pointer_cast<source_displaycapture4>(this->shared_from_this())));
    return temp;
}

HRESULT source_displaycapture4::initialize(
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
    CHECK_HR(hr = dxgiadapter->EnumOutputs(MONITOR_INDEX, &output));
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

    // allocate and start the capture thread
    assert(!this->capture_thread);
    assert(!this->capture_thread_clock);
    this->capture_thread_clock.reset(new presentation_clock);
    this->capture_thread.Attach(new thread_capture(
        std::dynamic_pointer_cast<source_displaycapture4>(
        this->shared_from_this()), this->capture_thread_clock));
    this->capture_thread_clock->clock_start(0);

    return hr;
}

void source_displaycapture4::capture_frame(LARGE_INTEGER start_time)
{
    scoped_lock lock(this->mutex);

    CComPtr<IDXGIResource> frame;
    CComPtr<ID3D11Texture2D> screen_frame;
    CComPtr<IDXGIKeyedMutex> frame_mutex;
    DXGI_OUTDUPL_FRAME_INFO frame_info = {0};

    HRESULT hr = S_OK;
    
    // when playing a fullscreen game the releaseframe can be here;
    // in desktop mode it must be at the end so that every change can be captured
    /*this->output_duplication->ReleaseFrame();*/
    CHECK_HR(hr = this->output_duplication->AcquireNextFrame(INFINITE, &frame_info, &frame));
    CHECK_HR(hr = frame->QueryInterface(&screen_frame));

    if(frame_info.LastPresentTime.QuadPart != 0)
    {
        frame_info.LastPresentTime.QuadPart -= start_time.QuadPart;
        frame_info.LastPresentTime.QuadPart *= 1000000 * 10;
        frame_info.LastPresentTime.QuadPart /= pc_frequency.QuadPart;
        this->samples[this->current_frame]->timestamp = frame_info.LastPresentTime.QuadPart;
    }
    else
        this->samples[this->current_frame]->timestamp = this->get_device_clock()->get_current_time();

    UINT64 key = 1;
    // allocate buffer
    if(!this->screen_frame[this->current_frame])
    {
        D3D11_TEXTURE2D_DESC screen_frame_desc;
        screen_frame->GetDesc(&screen_frame_desc);
        screen_frame_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        CHECK_HR(hr = this->d3d11dev->CreateTexture2D(
            &screen_frame_desc, NULL, &this->screen_frame[this->current_frame]));

        CComPtr<IDXGIResource> idxgiresource;
        CHECK_HR(hr = this->screen_frame[this->current_frame]->QueryInterface(&idxgiresource));
        HANDLE h;
        CHECK_HR(hr = idxgiresource->GetSharedHandle(&h));
        this->samples[this->current_frame]->frame = h;
        key = 0;
    }

    // copy
    CHECK_HR(hr = this->screen_frame[this->current_frame]->QueryInterface(&frame_mutex));
    CHECK_HR(hr = frame_mutex->AcquireSync(key, INFINITE));
    this->d3d11devctx->CopyResource(this->screen_frame[this->current_frame], screen_frame);
    if(key == 0)
        key = 1;
    CHECK_HR(hr = frame_mutex->ReleaseSync(key));

done:
    this->output_duplication->ReleaseFrame();

    if(hr == DXGI_ERROR_WAIT_TIMEOUT)
        std::cout << "FRAME IS NULL------------------" << std::endl;
    else if(FAILED(hr))
        throw std::exception();

    // dispatch all pending requests
    this->dispatch_requests();

    this->current_frame = (this->current_frame + 1) % SAMPLE_DEPTH;
}

void source_displaycapture4::dispatch_requests()
{
    stream_displaycapture4_cb callback;
    media_sample_t sample;

    scoped_lock lock(this->requests_mutex);
    
loop:
    for(auto it = this->requests.begin(); it != this->requests.end(); it++)
    {
        callback = *it;
        bool too_new;
        sample = this->capture_frame(callback.device_request_time, too_new);
        if(too_new)
            continue;

        this->requests.erase(it);

        callback.cb->process_sample(sample, callback.request_time);
        callback.cb->Release();
        goto loop;
    }
}

media_sample_t source_displaycapture4::capture_frame(time_unit timestamp, bool& too_new)
{
    time_unit diff = std::numeric_limits<time_unit>::max();
    size_t index = 0;
    too_new = true;
    // TODO: fetching 64 bit integer might not be an atomic operation
    for(size_t i = 0; i < SAMPLE_DEPTH; i++)
    {
        const time_unit new_diff = timestamp - this->samples[i]->timestamp;
        if(diff > std::abs(new_diff))
        {
            index = i;
            diff = std::abs(new_diff);
        }
        if(new_diff < 0)
            too_new = false;
    }
    if(diff > MAX_DIFF)
        return NULL;

    return this->samples[index];
}

presentation_clock_t source_displaycapture4::get_device_clock()
{
    return presentation_clock_t(std::atomic_load(&this->capture_thread_clock));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture4::stream_displaycapture4(const source_displaycapture4_t& source) : 
    source(source), 
    callback(this, &stream_displaycapture4::capture_cb)
{
}

HRESULT stream_displaycapture4::capture_cb(IMFAsyncResult*)
{
    scoped_lock lock(this->mutex);

    // TODO: this probably can be moved to request_sample function
    while(!this->requests.empty())
    {
        const time_unit request_time = this->requests.front();

        // convert the topology's time to device time
        presentation_clock_t device_clock = this->source->get_device_clock();
        presentation_clock_t clock;
        if(!this->get_clock(clock))
            return S_OK;
        const time_unit time_diff = device_clock->get_current_time() - clock->get_current_time();
        const time_unit device_time = request_time + time_diff;

        bool too_new;
        media_sample_t sample = this->source->capture_frame(device_time /*- LAG_BEHIND*/, too_new);
        if(too_new)
        {
            // move this request to source's request queue
            scoped_lock lock(this->source->requests_mutex);
            stream_displaycapture4_cb callback = {this, device_time, request_time};
            this->source->requests.push_back(callback);
            this->AddRef();

            /*std::cout << "--REQUEST WAS TOO NEW--" << std::endl;*/
            return S_OK;
        }

        this->requests.pop_front();
        if(!sample)
        {
            std::cout << "--SUITABLE FRAMES NOT FOUND--" << std::endl;
            return S_OK;
        }
        this->process_sample(sample, request_time);
    }
    
    return S_OK;
}

media_stream::result_t stream_displaycapture4::request_sample(time_unit request_time)
{
    // convert the topology's time to device time
    presentation_clock_t device_clock = this->source->get_device_clock();
    presentation_clock_t clock;
    if(!this->get_clock(clock))
        return FATAL_ERROR;
    const time_unit time_diff = device_clock->get_current_time() - clock->get_current_time();
    const time_unit device_time = request_time + time_diff;

    bool too_new;
    media_sample_t sample = this->source->capture_frame(device_time /*- LAG_BEHIND*/, too_new);
    //if(too_new)
    //{
    //    // move the request to source's request queue
    //    scoped_lock lock(this->source->requests_mutex);
    //    if(this->source->requests.size() >= SAMPLE_DEPTH)
    //    {
    //        std::cout << "--SAMPLE REQUEST DROPPED--" << std::endl;
    //        return OK;
    //    }

    //    stream_displaycapture4_cb callback = {this, device_time, request_time};
    //    this->source->requests.push_back(callback);
    //    this->AddRef();

    //    return OK;
    //}

    return this->process_sample(sample, request_time);

    //{
    //    scoped_lock lock(this->mutex);
    //    if(this->requests.size() >= SAMPLE_DEPTH)
    //    {
    //        // do not assign a new work item for this request since the queue is already full
    //        /*this->requests.pop();*/
    //        std::cout << "--SAMPLE REQUEST DROPPED--" << std::endl;
    //        return OK;
    //    }
    //    this->requests.push_back(request_time);
    //}

    //// TODO: in case of audio(and maybe webcams), if the request time is greater
    //// than in the current queue, the request item should wait for the new samples to arrive;
    //// this compensates for the sample lag that might happen in usb devices;
    //// in that case the source should push the sample from the request queue to downstream
    //// (assuming that the samples arrive in the source in chronological order);
    //// (or not for the source's logic to stay relatively simple)

    //// otherwise the samples are pulled from the source's queue
    //const HRESULT hr = MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &this->callback, NULL);
    //if(hr != S_OK)
    //    return FATAL_ERROR;

    //return OK;
}

media_stream::result_t stream_displaycapture4::process_sample(
    const media_sample_t& sample, time_unit request_time)
{
    // add the timestamp and lock the frame
    presentation_clock_t clock;
    if(!this->get_clock(clock))
        return FATAL_ERROR;

    // TODO: the timestamp should have the presentation time instead of the
    // device time

    /*sample->timestamp = request_time;*//*clock->get_current_time();*/

    // TODO: the lock mechanism is converted to the request packet mechanism
    sample->mutex.lock();

    // pass the sample to downstream
    this->source->session->give_sample(this, sample, request_time, true);
    return OK;
}