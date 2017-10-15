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

#define MAX_DIFF std::numeric_limits<time_unit>::max()/*(FPS60_INTERVAL / 2 + FPS60_INTERVAL / 4)*/

source_displaycapture4::thread_capture::thread_capture(source_displaycapture4_t& source) : 
    source(source),
    running(false)
{
    this->callback.Attach(new async_callback_t(&source_displaycapture4::thread_capture::capture_cb));
    if(FAILED(MFAllocateWorkQueue(&this->work_queue)))
        throw std::exception();
}

source_displaycapture4::thread_capture::~thread_capture()
{
    /*this->unregister_sink();*/
    MFUnlockWorkQueue(this->work_queue);
}

bool source_displaycapture4::thread_capture::on_clock_start(time_unit t, int packet_number)
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

    const HRESULT hr = this->callback->mf_put_work_item(
        this->shared_from_this<thread_capture>(), this->work_queue);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
}

void source_displaycapture4::thread_capture::capture_cb(void*)
{
    source_displaycapture4_t source;

    if(!this->running || !this->get_source(source))
        return;

    // do not schedule a new time until the old capture has succeeded,
    // because the capturing might exceed the next scheduled time;
    // do not schedule a new time until the frame that is going to be modified has become
    // available aswell
    presentation_clock_t clock;
    if(this->get_clock(clock))
    {
        // wait until the sample has become available again
        const int current_frame = source->current_frame;
        source->samples[current_frame]->lock_sample();
        // TODO: sinks should request the oldest frame
        source->capture_frame(clock->get_start_time_internal());
        source->samples[current_frame]->unlock_sample();

        this->schedule_new(this->due_time);
    }
    else
        this->on_clock_stop(0);
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
                    std::cout << "--FRAME DROPPED-- @ monitor " << this->monitor_index << std::endl;

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
    media_source(session), current_frame(0), null_sample(new media_sample_texture)
{
    for(size_t i = 0; i < SAMPLE_DEPTH; i++)
    {
        this->samples[i].reset(new media_sample_texture);
        this->samples[i]->timestamp = 0;
    }
    this->null_sample->timestamp = 0;
}

source_displaycapture4::~source_displaycapture4()
{
    if(this->capture_thread)
        this->capture_thread->running = false;
}

media_stream_t source_displaycapture4::create_stream()
{
    return media_stream_t(
        new stream_displaycapture4(this->shared_from_this<source_displaycapture4>()));
}

HRESULT source_displaycapture4::initialize(UINT output_index, const CComPtr<ID3D11Device>& d3d11dev2)
{
    HRESULT hr;
    CComPtr<IDXGIDevice> dxgidev;
    CComPtr<IDXGIAdapter> dxgiadapter;
    CComPtr<IDXGIOutput> output;
    CComPtr<IDXGIOutput1> output1;

    this->d3d11dev2 = d3d11dev2;

    // create d3d11 device
    CHECK_HR(hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION, &this->d3d11dev, NULL, &this->d3d11devctx));

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

    // allocate and start the capture thread
    assert(!this->capture_thread);
    assert(!this->capture_thread_clock);
    this->capture_thread_clock.reset(new presentation_clock);
    this->capture_thread.reset(new thread_capture(this->shared_from_this<source_displaycapture4>()));
    this->capture_thread->monitor_index = output_index;
    this->capture_thread->register_sink(this->capture_thread_clock);
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
    // TODO: this function should not block so that resource freeing is possible
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
    // allocate texture
    if(!this->samples[this->current_frame]->texture)
    {
        CComPtr<IDXGIResource> idxgiresource;
        CComPtr<IDXGIKeyedMutex> mutex;
        CComPtr<ID3D11Texture2D> texture;

        D3D11_TEXTURE2D_DESC screen_frame_desc;
        screen_frame->GetDesc(&screen_frame_desc);
        screen_frame_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        screen_frame_desc.Usage = D3D11_USAGE_DEFAULT;
        CHECK_HR(hr = this->d3d11dev2->CreateTexture2D(
            &screen_frame_desc, NULL, &this->samples[this->current_frame]->texture));
        texture = this->samples[this->current_frame]->texture;

        CHECK_HR(hr = texture->QueryInterface(&idxgiresource));
        HANDLE h;
        CHECK_HR(hr = idxgiresource->GetSharedHandle(&h));
        CHECK_HR(hr = texture->QueryInterface(&mutex));

        // set the properties of the allocated sample
        this->samples[this->current_frame]->shared_handle = h;
        this->samples[this->current_frame]->mutex = mutex;
        this->samples[this->current_frame]->resource = idxgiresource;

        key = 0;
    }

    // copy
    {
        CComPtr<ID3D11Texture2D> texture;

        CHECK_HR(hr = this->d3d11dev->OpenSharedResource(
            this->samples[this->current_frame]->shared_handle,
            __uuidof(ID3D11Texture2D), (void**)&texture));

        CHECK_HR(hr = texture->QueryInterface(&frame_mutex));
        CHECK_HR(hr = frame_mutex->AcquireSync(key, INFINITE));
        this->d3d11devctx->CopyResource(texture, screen_frame);
        if(key == 0)
            key = 1;
        CHECK_HR(hr = frame_mutex->ReleaseSync(key));
    }

done:
    this->output_duplication->ReleaseFrame();

    if(hr == DXGI_ERROR_WAIT_TIMEOUT)
        std::cout << "FRAME IS NULL------------------" << std::endl;
    else if(FAILED(hr))
        throw std::exception();

    this->current_frame = (this->current_frame + 1) % SAMPLE_DEPTH;
}

media_sample_t source_displaycapture4::capture_frame(time_unit timestamp, bool& too_new)
{
    time_unit diff = std::numeric_limits<time_unit>::max();
    size_t index = -1;
    too_new = true;

    for(size_t i = 0; i < SAMPLE_DEPTH; i++)
    {
        // checks whether the sample is available and locks it
        if(!this->samples[i]->try_lock_sample())
            continue;

        const time_unit new_diff = timestamp - this->samples[i]->timestamp;
        if(diff > std::abs(new_diff))
        {
            if(index != -1)
                this->samples[index]->unlock_sample();
            index = i;
            diff = std::abs(new_diff);
        }
        else
            this->samples[i]->unlock_sample();
        if(new_diff < 0)
            too_new = false;
    }
    if(diff > MAX_DIFF)
        return NULL;

    // TODO: null sample should be implemented that don't have
    // a locking mechanism
    if(index == -1)
    {
        // locking isn't necessary for null sample so just try to lock it
        /*this->null_sample->try_lock_sample();*/
        return this->null_sample;
    }
    else
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
    source(source)
{
}

stream_displaycapture4::~stream_displaycapture4()
{
}

media_stream::result_t stream_displaycapture4::request_sample(
    request_packet& rp, const media_stream*)
{
    // convert the topology's time to device time
    presentation_clock_t device_clock = this->source->get_device_clock();
    presentation_clock_t clock;
    if(!this->get_clock(clock))
        return FATAL_ERROR;
    const time_unit time_diff = device_clock->get_current_time() - clock->get_current_time();
    const time_unit device_time = rp.request_time + time_diff;

    bool too_new;
    // capture frame locks the sample
    media_sample_t sample = this->source->capture_frame(device_time, too_new);
    media_sample_view_t sample_view(new media_sample_view(sample, true));

    return this->process_sample(sample_view, rp);
}

media_stream::result_t stream_displaycapture4::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    // add the timestamp and lock the frame
    presentation_clock_t clock;
    if(!this->get_clock(clock))
        return FATAL_ERROR;

    // TODO: the timestamp should have the presentation time instead of the
    // device time

    // (the sample is already locked when it comes here)

    // pass the sample to downstream
    this->source->session->give_sample(this, sample_view, rp, true);
    return OK;
}