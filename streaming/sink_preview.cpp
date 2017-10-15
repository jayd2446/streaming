#include "sink_preview.h"
//#include "source_displaycapture.h"
//#include "source_displaycapture2.h"
//#include "source_displaycapture3.h"
#include "source_displaycapture4.h"
#include <Mferror.h>
#include <evr.h>
#include <iostream>
#include <mutex>
#include <avrt.h>

#pragma comment(lib, "Evr.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "Avrt.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#define LAG_BEHIND 1000000/*(FPS60_INTERVAL * 6)*/
extern LARGE_INTEGER pc_frequency;


sink_preview::sink_preview(const media_session_t& session) : 
    media_sink(session), drawn(false), requests_pending(0)
{
    for(int i = 0; i < QUEUE_MAX_SIZE; i++)
    {
        this->pending_streams[i].packet_number = -1;
        this->pending_streams[i].available = true;
    }
}

sink_preview::~sink_preview()
{
    HRESULT hr = this->sink_writer->Finalize();
    if(FAILED(hr))
        std::cout << "finalizing failed" << std::endl;
}

HRESULT sink_preview::create_output_media_type()
{
    HRESULT hr = S_OK;

    CHECK_HR(hr = MFCreateMediaType(&this->mpeg_file_type));
    CHECK_HR(hr = this->mpeg_file_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(hr = this->mpeg_file_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    CHECK_HR(hr = this->mpeg_file_type->SetUINT32(MF_MT_AVG_BITRATE, 6000*1000));
    CHECK_HR(hr = MFSetAttributeRatio(this->mpeg_file_type, MF_MT_FRAME_RATE, 60, 1));
    CHECK_HR(hr = MFSetAttributeSize(this->mpeg_file_type, MF_MT_FRAME_SIZE, 1920, 1080));
    CHECK_HR(hr = this->mpeg_file_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK_HR(hr = this->mpeg_file_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    CHECK_HR(hr = MFSetAttributeRatio(this->mpeg_file_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

done:
    return hr;
}

HRESULT sink_preview::create_input_media_type()
{
    HRESULT hr = S_OK;

    CHECK_HR(hr = MFCreateMediaType(&this->input_media_type));
    CHECK_HR(hr = this->input_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(hr = this->input_media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32));
    CHECK_HR(hr = MFSetAttributeRatio(this->input_media_type, MF_MT_FRAME_RATE, 60, 1));
    CHECK_HR(hr = MFSetAttributeSize(this->input_media_type, MF_MT_FRAME_SIZE, 1920, 1080));
    CHECK_HR(hr = this->input_media_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK_HR(hr = this->input_media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    CHECK_HR(hr = MFSetAttributeRatio(this->input_media_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

done:
    return hr;
}

HRESULT sink_preview::initialize_sink_writer()
{
    HRESULT hr = S_OK;

    CComPtr<IMFAttributes> sink_writer_attributes;

    // create output media type
    CHECK_HR(hr = this->create_output_media_type());
    // create input media type
    CHECK_HR(hr = this->create_input_media_type());

    // create file
    CHECK_HR(hr = MFCreateFile(
        MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, 
        L"test.mp4", &this->byte_stream));

    // configure the sink writer
    CHECK_HR(hr = MFCreateAttributes(&sink_writer_attributes, 1));
    CHECK_HR(hr = sink_writer_attributes->SetGUID(
        MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
    CHECK_HR(hr = sink_writer_attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    CHECK_HR(hr = sink_writer_attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, this->devmngr));

    // create sink writer
    CHECK_HR(hr = MFCreateSinkWriterFromURL(
        NULL, this->byte_stream, sink_writer_attributes, &this->sink_writer));

    // set the output stream format
    CHECK_HR(hr = this->sink_writer->AddStream(this->mpeg_file_type, &this->stream_index));

    // set the input stream format
    // TODO: this function accepts an encoding parameter
    CHECK_HR(hr = this->sink_writer->SetInputMediaType(this->stream_index, this->input_media_type, NULL));

    // tell the sink writer to start accepting data
    CHECK_HR(hr = this->sink_writer->BeginWriting());

done:
    return hr;
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

    // initialize devmngr
    CHECK_HR(hr = MFCreateDXGIDeviceManager(&this->reset_token, &this->devmngr));
    CHECK_HR(hr = this->devmngr->ResetDevice(this->d3d11dev, this->reset_token));

    // initialize sink writer
    CHECK_HR(hr = this->initialize_sink_writer());

done:
    if(FAILED(hr))
        throw std::exception();
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
    sink(sink), running(false)
{
    this->callback.Attach(new async_callback_t(&stream_preview::request_cb));
}

stream_preview::~stream_preview()
{
    /*this->unregister_sink();*/
}

bool stream_preview::on_clock_start(time_unit t, int packet_number)
{
    std::cout << "playback started" << std::endl;
    this->running = true;
    this->sink->packet_number = packet_number;
    this->scheduled_callback(t);
    return true;
}

void stream_preview::on_clock_stop(time_unit t)
{
    std::cout << "playback stopped" << std::endl;
    this->running = false;
    this->clear_queue();
}

DWORD task_index2 = 0;
HANDLE ret = 0;

void stream_preview::scheduled_callback(time_unit due_time)
{
    if(!this->running)
        return;
    
    // add a new request
    this->push_request(due_time);

    // initiate the request
    const HRESULT hr = this->callback->mf_put_work_item(
        this->shared_from_this<stream_preview>(), MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();

    // schedule a new time
    this->schedule_new(due_time);
}

//bool bb = true;

void stream_preview::schedule_new(time_unit due_time)
{
    presentation_clock_t t;
    this->get_clock(t);
    if(t)
    {
        /*if(!bb)
            return;*/

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

        // TODO: by setting the fps lag in the middle of cache,
        // new frame can be requested even if the scheduled time is late

        /*last_scheduled_time = scheduled_time;*/
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
    }
}

void stream_preview::push_request(time_unit t)
{
    std::lock_guard<std::recursive_mutex> lock(this->mutex);
    sink_preview::request_t request;
    request.request_time = t - LAG_BEHIND;
    request.timestamp = t;
    request.packet_number = this->sink->packet_number++;
    this->sink->requests.push(request);
}

void stream_preview::request_cb(void*)
{
    sink_preview::request_t request;
    request_packet rp;
    {
        std::lock_guard<std::recursive_mutex> lock(this->mutex);
        request = this->sink->requests.front();
        this->sink->requests.pop();
    }
    
    // TODO: there's still a chance that the requests queue will over saturate
    // which implies a very massive lag
    if(this->sink->requests_pending >= QUEUE_MAX_SIZE)
    {
        std::cout << "--SAMPLE REQUEST DROPPED IN STREAM_PREVIEW--" << std::endl;
        return;
    }

    // wait for the source texture cache to saturate
    if(request.request_time >= 0)
    {
        this->sink->requests_pending++;
        rp.request_time = request.request_time;
        rp.timestamp = request.timestamp;
        rp.packet_number = request.packet_number;
        if(this->request_sample(rp) == FATAL_ERROR)
        {
            this->sink->requests_pending--;
            this->running = false;
        }
    }
}

bool stream_preview::get_clock(presentation_clock_t& clock)
{
    return this->sink->session->get_current_clock(clock);
}

media_stream::result_t stream_preview::request_sample(request_packet& rp, const media_stream*)
{
    // dispatch the request to another equivalent topology branch
    media_stream::result_t res = OK;
    for(int i = 0; i < QUEUE_MAX_SIZE; i++)
    {
        if(this->sink->pending_streams[i].available)
        {
            this->sink->pending_streams[i].available = false;
            this->sink->pending_streams[i].packet_number = rp.packet_number;

            res = this->sink->concurrent_streams[i]->request_sample(rp);
            return res;
        }
    }

    /*assert(false);*/
    return res;

    /*if(!this->sink->session->request_sample(this, rp, true))
        return FATAL_ERROR;*/
}

media_stream::result_t stream_preview::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    // schedule the sample
    // 5000000 = half a second

    // render the sample to the backbuffer here

    // TODO: drawing etc should be put to a work queue

    static HANDLE last_frame;
    static time_unit last_request_time = 0;
    HRESULT hr = S_OK;

    media_sample_texture_t sample = sample_view->get_sample<media_sample_texture>();
    if(sample)
    {
        CComPtr<ID3D11Texture2D> texture = 
            sample_view->get_sample<media_sample_texture>()->texture;
        if(texture)
        {
            /*goto out;*/
            CComPtr<IDXGISurface> surface;
            CComPtr<IDXGIKeyedMutex> mutex;
            CComPtr<IMFMediaBuffer> buffer;
            CComPtr<IMFSample> sample2;
            CComPtr<IMF2DBuffer> buffer2d;

            hr = texture->QueryInterface(&surface);
            hr = surface->QueryInterface(&mutex);
            hr = mutex->AcquireSync(1, INFINITE);

            hr = MFCreateDXGISurfaceBuffer(
                IID_ID3D11Texture2D, texture, 0, FALSE, &buffer);
            // the length must be set for the buffer so that sink writer doesn't fail
            // (documentation was lacking for this case)
            hr = buffer->QueryInterface(&buffer2d);
            DWORD len;
            hr = buffer2d->GetContiguousLength(&len);
            hr = buffer->SetCurrentLength(len);
            hr = MFCreateVideoSampleFromSurface(NULL, &sample2);
            hr = sample2->AddBuffer(buffer);
            const LONGLONG timestamp = sample_view->get_sample()->timestamp;
            static LONGLONG timestamp_ = 0;
            hr = sample2->SetSampleTime(timestamp_);
            timestamp_ += FPS60_INTERVAL;
            /*hr = sample2->SetSampleDuration(FPS60_INTERVAL);*/
            /*hr = this->sink->sink_writer->WriteSample(this->sink->stream_index, sample2);*/

            /*mutex->ReleaseSync(1);*/
            if(FAILED(hr))
                throw std::exception();

            // TODO: decide if the mutex is necessary here
            std::lock_guard<std::mutex> lock(this->render_mutex);
            this->sink->drawn = true;
            //CComPtr<IDXGISurface> surface;
            //hr = this->sink->d3d11dev->OpenSharedResource(
            //    sample->frame, __uuidof(IDXGISurface), (void**)&surface);
            CComPtr<ID2D1Bitmap1> frame;
            //CComPtr<IDXGIKeyedMutex> frame_mutex;
            //hr = surface->QueryInterface(&frame_mutex);
            //frame_mutex->AcquireSync(1, INFINITE);
            hr = this->sink->d2d1devctx->CreateBitmapFromDxgiSurface(
                surface,
                D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                &frame);

            //// 10000000

            this->sink->d2d1devctx->BeginDraw();
            this->sink->d2d1devctx->DrawBitmap(frame);
            hr = this->sink->d2d1devctx->EndDraw();
            mutex->ReleaseSync(1);

            /*this->sink->drawn = false;*/
        }
        else
            this->sink->drawn = false;
    }
    
    if(last_request_time > rp.request_time)
    {
        std::cout << "OUT OF ORDER FRAME" << std::endl;
    }

    /*last_frame = sample_view->get_sample()->frame;*/
    last_request_time = rp.request_time;

    // unlock the frame
    /*sample->unlock_sample();*/

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

    // make the stream available
    for(int i = 0; i < QUEUE_MAX_SIZE; i++)
    {
        if(this->sink->pending_streams[i].packet_number == rp.packet_number)
            this->sink->pending_streams[i].available = true;
    }

    this->sink->requests_pending--;

    /*std::cout << "frame time: " << rp.request_time << std::endl;*/

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