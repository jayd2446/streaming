#include "SourceDesktop.h"
#include "StreamDxgiTexture.h"
#include <mfapi.h>
#include <evr.h>
#include <dxgi.h>
#include <d3d11.h>
#include <d3d9types.h>
#include <iostream>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "evr")
#pragma comment(lib, "dxguid")

// DXGI_FORMAT_B8G8R8A8_UNORM equals D3DFMT_A8R8G8B8
// desktop duplication api contains frames always in the DXGI_FORMAT_B8G8R8A8_UNORM format
#define STREAM_FORMAT D3DFMT_A8R8G8B8
#define STREAM_WIDTH 1920
#define STREAM_HEIGHT 1080
#define STREAM_FPS {60, 1}
#define STREAM_RATIO {16, 9}
#define STREAM_ID 0x17af79bc
// TODO: needs to be exactly 60 fps scheduling
#define WORK_ITEM_TIMEOUT_MS 16 // ~60fps
#define ACQUIRE_FRAME_TIMEOUT_MS 16



HRESULT SourceDesktop::CreateInstance(SourceDesktop** source)
{
    if(!source)
        return E_POINTER;

    HRESULT hr = S_OK;
    SourceDesktop* src = new SourceDesktop(hr);
    if(!src)
        return E_OUTOFMEMORY;

    if(SUCCEEDED(hr))
    {
        *source = src;
        (*source)->AddRef();
    }

    SAFE_RELEASE(src);
    return hr;
}

SourceDesktop::SourceDesktop(HRESULT& hr) : 
    event_queue(NULL), state(INVALID), OnProcessQueue_cb(this, &SourceDesktop::ProcessQueueAsync),
    OnRequestSample_cb(this, &SourceDesktop::OnRequestSample), screen_frame_released(true),
    OnSampleReleased_cb(this, &SourceDesktop::OnSampleReleased)
{
    this->start_time.QuadPart = 0;

    hr = MFCreateAttributes(&this->attributes, 1);
    if(FAILED(hr))
        return;
    hr = MFCreateEventQueue(&this->event_queue);
    if(FAILED(hr))
        return;
    hr = this->Initialize();
}

SourceDesktop::~SourceDesktop()
{
    if(this->state != SHUTDOWN_STATE)
        this->Shutdown();
}

HRESULT SourceDesktop::QueryInterface(REFIID riid, void** ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(SourceDesktop, IMFMediaSource),
        QITABENT(SourceDesktop, IMFMediaSourceEx),
        QITABENT(SourceDesktop, IMFMediaEventGenerator),
        QITABENT(SourceDesktop, IMFRealTimeClientEx),
        {0}
    };
    return QISearch(this, qit, riid, ppv);
}

HRESULT SourceDesktop::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    HRESULT hr = S_OK; 
    CHECK_HR(hr = this->CheckShutdown()); 
    CHECK_HR(hr = this->event_queue->GetEvent(dwFlags, ppEvent)); 
done: 
    return hr;
}

HRESULT SourceDesktop::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    HRESULT hr = S_OK; 
    CHECK_HR(hr = this->CheckShutdown()); 
    CHECK_HR(hr = this->event_queue->BeginGetEvent(pCallback, punkState)); 
done: 
    return hr;
}

HRESULT SourceDesktop::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    HRESULT hr = S_OK; 
    CHECK_HR(hr = this->CheckShutdown()); 
    CHECK_HR(hr = this->event_queue->EndGetEvent(pResult, ppEvent)); 
done: 
    return hr;
}

HRESULT SourceDesktop::QueueEvent(MediaEventType met, REFGUID guidExtendedType, 
    HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    HRESULT hr = S_OK; 
    CHECK_HR(hr = this->CheckShutdown()); 
    CHECK_HR(hr = this->event_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue)); 
done: 
    return hr;
}

HRESULT SourceDesktop::CreatePresentationDescriptor(IMFPresentationDescriptor** pd)
{
    if(!pd)
        return E_POINTER;

    HRESULT hr = S_OK;
    CHECK_HR(hr = this->CheckShutdown());
    if(!this->descriptor)
        CHECK_HR(hr = MF_E_NOT_INITIALIZED);
    CHECK_HR(hr = this->descriptor->Clone(pd));

done:
    return hr;
}

HRESULT SourceDesktop::GetCharacteristics(DWORD* pdwCharacteristics)
{
    if(!pdwCharacteristics)
        return E_POINTER;

    HRESULT hr = S_OK;
    CHECK_HR(hr = this->CheckShutdown());

    // TODO: add pause characteristic
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE | MFMEDIASOURCE_DOES_NOT_USE_NETWORK;
done:
    return hr;
}

//HRESULT SourceDesktop::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes)
//{
//    if(ppAttributes == NULL)
//        return E_POINTER;
//    if(dwStreamIdentifier != STREAM_ID)
//        return MF_E_INVALIDSTREAMNUMBER;
//
//    HRESULT hr = S_OK;
//
//    CComPtr<IMFStreamDescriptor> descriptor;
//    CComPtr<IMFAttributes> attributes;
//    CHECK_HR(hr = this->stream->GetStreamDescriptor(&descriptor));
//    UINT32 item_count;
//    CHECK_HR(hr = descriptor->GetCount(&item_count));
//    CHECK_HR(hr = MFCreateAttributes(&attributes, item_count));
//    CHECK_HR(hr = descriptor->CopyAllItems(attributes));
//    *ppAttributes = attributes;
//    (*ppAttributes)->AddRef();
//
//done:
//    return hr;
//}

// TODO: return e_not_impl on both getsourceattributes and getstreamattributes
HRESULT SourceDesktop::GetSourceAttributes(IMFAttributes** ppAttributes)
{
    if(ppAttributes == NULL)
        return E_POINTER;

    *ppAttributes = this->attributes;
    (*ppAttributes)->AddRef();

    return S_OK;
}

HRESULT SourceDesktop::GetStreamAttributes(DWORD, IMFAttributes** ppAttributes)
{
    return this->GetSourceAttributes(ppAttributes);
}

HRESULT SourceDesktop::SetD3DManager(IUnknown* pManager)
{
    HRESULT hr = S_OK;

    if(pManager == NULL)
        return E_POINTER;

    this->device_manager = NULL;
    hr = pManager->QueryInterface(&this->device_manager);
    return hr;
}

HRESULT SourceDesktop::Shutdown()
{
    ScopedLock lock(this->op_mutex);

    HRESULT hr = S_OK;
    CHECK_HR(hr = this->CheckShutdown());

    // lock so that async operations aren't running when shutting down
    
    // change the state temporarily so that it's easier for shutdown() method
    // to acquire the lock
    const State_t state = this->state;
    this->state = SHUTDOWN_STATE;
    this->state = state;

    if(this->stream)
        this->stream->Shutdown();
    if(this->event_queue)
        this->event_queue->Shutdown();

    // release the stream object here to break the circular reference
    this->state = SHUTDOWN_STATE;
    this->stream = NULL;
    this->event_queue = NULL;
    this->descriptor = NULL;
    {
        ScopedLock op_lock(this->op_queue_mutex);
        op_queue_t().swap(this->op_queue);
    }
done:
    return hr;
}

HRESULT SourceDesktop::ScheduleSampleSubmit(bool cancel)
{
    if(cancel)
        return MFCancelWorkItem(this->schedule_item_key);
    else
        return MFScheduleWorkItem(
        &this->OnRequestSample_cb, NULL, -WORK_ITEM_TIMEOUT_MS, &this->schedule_item_key);
}

HRESULT SourceDesktop::QueueAsyncOperation(Operation& op)
{
    {
        ScopedLock lock(this->op_queue_mutex);
        this->op_queue.push(op);
    }
    return this->ProcessQueue();
}

HRESULT SourceDesktop::ProcessQueue()
{
    ScopedLock lock(this->op_queue_mutex);
    if(!this->op_queue.empty())
        return MFPutWorkItem2(this->dwMultithreadedWorkQueueId, this->lWorkItemBasePriority, 
        &this->OnProcessQueue_cb, NULL);
    return S_OK;
}

HRESULT SourceDesktop::ProcessQueueAsync(IMFAsyncResult *pResult)
{
    ScopedLock op_lock(this->op_mutex);
    this->op_queue_mutex.Lock();

    if(!this->op_queue.empty())
    {
        Operation op = this->op_queue.front();
        this->op_queue.pop();
        this->op_queue_mutex.Unlock();

        // TODO: decide if should return the onasyncops return code here
        this->OnAsyncOp(op);
        return S_OK;
    }

    this->op_queue_mutex.Unlock();
    return S_OK;
}

void SourceDesktop::OnAsyncOp(Operation& op)
{
    if(this->state == SHUTDOWN_STATE)
        return;

    HRESULT hr = S_OK;
    switch(op.op)
    {
    case START:
        hr = this->OnStart(op);
        break;
    case STOP:
        hr = this->OnStop(op);
        break;
    /*case PAUSE:
        this->OnPause(op);
        break;*/
    }

    if(FAILED(hr))
        this->QueueEvent(MEError, GUID_NULL, hr, NULL);

    //// invoke the remaining operations aswell if there's any
    //this->ProcessQueue();
}

HRESULT SourceDesktop::Initialize()
{
    assert(!this->descriptor);
    assert(this->state == INVALID);

    HRESULT hr = S_OK;

    // create the media type for the stream descriptor
    CComPtr<IMFStreamDescriptor> stream_descriptor;
    CComPtr<IMFMediaType> type;
    CComPtr<IMFMediaTypeHandler> handler;
    MFRatio framerate = STREAM_FPS;
    MFRatio pixel_aspect_ratio = STREAM_RATIO;
    CHECK_HR(hr = CreateUncompressedVideoType(
        STREAM_FORMAT, STREAM_WIDTH, STREAM_HEIGHT,
        MFVideoInterlace_Progressive,
        framerate, pixel_aspect_ratio,
        &type));
    // create the media descriptor
    CHECK_HR(hr = MFCreateStreamDescriptor(STREAM_ID, 1, &type.p, &stream_descriptor));
    // set the default media type on the stream handler
    CHECK_HR(hr = stream_descriptor->GetMediaTypeHandler(&handler));
    CHECK_HR(hr = handler->SetCurrentMediaType(type));

    // create the stream
    this->stream.Attach(new StreamDxgiTexture(this, stream_descriptor, hr));
    CHECK_HR(hr);

    // create the presentation descriptor
    {
        CComPtr<IMFStreamDescriptor> stream_descriptor;
        CHECK_HR(hr = this->stream->GetStreamDescriptor(&stream_descriptor));
        CHECK_HR(hr = MFCreatePresentationDescriptor(1, &stream_descriptor.p, &this->descriptor));

        // select the stream to be used
        CHECK_HR(hr = this->descriptor->SelectStream(0));
    }

    this->state = STOPPED;

done:
    return hr;
}

HRESULT SourceDesktop::InitializeScreenCapture()
{
    if(!this->device_manager)
        return E_UNEXPECTED;

    HRESULT hr = S_OK;

    CComPtr<IDXGIOutput1> output1;
    CComPtr<IDXGIOutput> output;
    CComPtr<IDXGIAdapter> dxgiadapter;
    CComPtr<IDXGIDevice> dxgidev;

    // get the d3d11 device
    CComPtr<ID3D11Device> d3d11dev;
    HANDLE hdev = INVALID_HANDLE_VALUE; // TODO: decide if this handle should be closed

    CHECK_HR(hr = this->device_manager->OpenDeviceHandle(&hdev));
    hr = this->device_manager->GetVideoService(hdev, IID_ID3D11Device, (void**)&d3d11dev);
    if(hr == MF_E_DXGI_NEW_VIDEO_DEVICE)
    {
        CHECK_HR(hr = this->device_manager->CloseDeviceHandle(hdev));
        CHECK_HR(hr = this->device_manager->OpenDeviceHandle(&hdev));
        CHECK_HR(
            hr = this->device_manager->GetVideoService(hdev, IID_ID3D11Device, (void**)&d3d11dev));
    }
    else if(FAILED(hr))
        goto done;

    std::cout << "TODO: decide if the device handle should be closed" << std::endl;

    // get dxgi device
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
    this->screen_capture = NULL;
    CHECK_HR(hr = output1->DuplicateOutput(d3d11dev, &this->screen_capture));

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
        std::cout << "maximum number of desktop duplication api applications running" << std::endl;
    else if(FAILED(hr))
        std::cout << "unable to duplicate the output" << std::endl;

    return hr;
}

HRESULT SourceDesktop::OnPause(Operation& op)
{
    assert(false);
    // TODO:
    return S_OK;
}

HRESULT SourceDesktop::OnStop(Operation& op)
{
    HRESULT hr = S_OK;

    // stop the stream
    CHECK_HR(hr = this->stream->Stop());
    // cancel the scheduled sample push
    CHECK_HR(hr = this->ScheduleSampleSubmit(true));

    this->state = STOPPED;

done:
    // send the stopped event. this might include a failure code
    this->event_queue->QueueEventParamVar(MESourceStopped, GUID_NULL, hr, NULL);
    return hr;
}

HRESULT SourceDesktop::OnStart(Operation& op)
{
    assert(op.pd);

    // perform the async operation of Start()
    std::cout << "OnStart()" << std::endl;

    HRESULT hr = S_OK;

    // get the presentation descriptor that the client requested
    CComPtr<IMFPresentationDescriptor> descriptor = op.pd;

    // initialize the screen capture
    CHECK_HR(hr = this->InitializeScreenCapture());

    // source will only generate data on selected streams;
    // so it will compare the requested descriptor to the old descriptor
    // and send appropriate events to the stream;
    // in this source though there's only 1 stream and it must be active at all times
    CHECK_HR(
        hr = this->event_queue->QueueEventParamUnk(MEUpdatedStream, GUID_NULL, hr, this->stream));

    // start the stream; queueevent will create a copy of the propvariant struct
    CHECK_HR(hr = this->stream->QueueEvent(MEStreamStarted, GUID_NULL, S_OK, &op.startpos));

    // add the periodic work item which submits the samples to the stream
    CHECK_HR(hr = this->ScheduleSampleSubmit(false));

    // queue the started event; the event data is the start position
    this->start_time.QuadPart = 0;
    this->state = STARTED;
    CHECK_HR(
        hr = this->event_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &op.startpos));
done:
    if(FAILED(hr))
        this->event_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, hr, NULL);

    return hr;
}

HRESULT SourceDesktop::OnRequestSample(IMFAsyncResult* pResult)
{
    if(this->state == SHUTDOWN_STATE || this->state == STOPPED)
        return S_OK;

    HRESULT hr = S_OK;

    CComPtr<IMFTrackedSample> tracked_sample;
    CComPtr<IMFSample> sample;
    CComPtr<IMFMediaBuffer> buffer;
    CComPtr<ID3D11Texture2D> screen_frame;
    CComPtr<IDXGIResource> frame;

    // schedule the next sample push
    // TODO: this scheduling must be more accurate
    this->ScheduleSampleSubmit(false);

    // check if the screen buffer is still in use(desktop duplication api
    // needs the buffer to be released before it can be used again)
    if(!this->screen_frame_released)
    {
        std::cout << "frame dropped due to slow pipeline" << std::endl;
        return S_OK;
    }

    // TODO: handle device lost
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    // release the old frame
    hr = this->screen_capture->ReleaseFrame();
    /*assert(SUCCEEDED(hr));
    CHECK_HR(hr);*/
    // get the new frame;
    // this fails if the previous frame is still in the pipeline;
    // that means the stream dropped a frame
    CHECK_HR(hr = this->screen_capture->AcquireNextFrame(
        ACQUIRE_FRAME_TIMEOUT_MS, &frame_info, &frame));
    this->screen_frame_released = false;
    CHECK_HR(hr = frame->QueryInterface(&screen_frame));

    // create a video sample which supports imftrackedsample
    CHECK_HR(hr = MFCreateVideoSampleFromSurface(NULL, &sample));
    // create a video buffer for the sample
    CHECK_HR(hr = MFCreateDXGISurfaceBuffer(
        IID_ID3D11Texture2D, screen_frame, 0, FALSE, &buffer));
    // add the buffer to the video sample
    CHECK_HR(hr = sample->AddBuffer(buffer));
    //CHECK_HR(hr = MFCreateVideoSampleFromSurface(screen_frame, &sample));

    // query for the imftrackedsample interface and set the allocator
    CHECK_HR(hr = sample->QueryInterface(&tracked_sample));
    CHECK_HR(hr = tracked_sample->SetAllocator(&this->OnSampleReleased_cb, NULL));

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
    // in 100 nanosecond units
    CHECK_HR(hr = sample->SetSampleTime(nanosecond_100.QuadPart));

    // deliver
    CHECK_HR(hr = this->stream->DeliverPayload(sample));

done:
    if(FAILED(hr))
        this->QueueEvent(MEError, GUID_NULL, hr, NULL);

    return hr;
}

HRESULT SourceDesktop::OnSampleReleased(IMFAsyncResult* pResult)
{
    this->screen_frame_released = false;
    /*std::cout << "frame released" << std::endl;*/
    return S_OK;
}

HRESULT SourceDesktop::Start(
    IMFPresentationDescriptor *pPresentationDescriptor,
    const GUID *pguidTimeFormat,
    const PROPVARIANT *pvarStartPosition)
{
    assert(this->device_manager);

    if(!this->device_manager)
        return E_UNEXPECTED;

    HRESULT hr = S_OK;
    Operation op;
    CComPtr<IMFStreamDescriptor> stream_descriptor;

    if(pvarStartPosition == NULL || pPresentationDescriptor == NULL)
        return E_INVALIDARG;

    // check the time format
    if(pguidTimeFormat != NULL && *pguidTimeFormat != GUID_NULL)
        return MF_E_UNSUPPORTED_TIME_FORMAT;

    // check the data type of the start position
    if(pvarStartPosition->vt != VT_I8 && pvarStartPosition->vt != VT_EMPTY)
        return MF_E_UNSUPPORTED_TIME_FORMAT;

    // check if this is a seek request, which is not supported
    if(pvarStartPosition->vt == VT_I8)
        // if the current state is stopped, then position 0 is valid
        if(this->state != STOPPED || pvarStartPosition->hVal.QuadPart != 0)
            return MF_E_INVALIDREQUEST;

    // check if source is shutdown or not initialized
    CHECK_HR(hr = this->CheckShutdown());
    CHECK_HR(hr = this->CheckInitialized());

    // TODO: sanity check on presentation descriptor
    // CHECK_HR(hr = ValidatePresentationDescriptor(pPresentationDescriptor));

    // check that there's only 1 stream active and it is the stream that is managed
    // by this source
    BOOL selected;
    DWORD stream_id;
    CHECK_HR(hr = pPresentationDescriptor->GetStreamDescriptorByIndex(0, &selected, &stream_descriptor));
    CHECK_HR(hr = stream_descriptor->GetStreamIdentifier(&stream_id));
    if(stream_id != STREAM_ID || !selected)
        return MF_E_INVALIDREQUEST;

    // complete the operation asynchronously
    op.op = START;
    op.pd = pPresentationDescriptor;
    op.startpos = *pvarStartPosition;
    CHECK_HR(hr = this->QueueAsyncOperation(op));

done:
    return hr;
}

HRESULT SourceDesktop::Stop()
{
    HRESULT hr = S_OK;
    Operation op;

    CHECK_HR(hr = this->CheckShutdown());
    CHECK_HR(hr = this->CheckInitialized());
    op.op = STOP;
    CHECK_HR(hr = this->QueueAsyncOperation(op));

done:
    return hr;
}

HRESULT CreateUncompressedVideoType(
    DWORD                fccFormat,  // FOURCC or D3DFORMAT value.     
    UINT32               width, 
    UINT32               height,
    MFVideoInterlaceMode interlaceMode,
    const MFRatio&       frameRate,
    const MFRatio&       par,
    IMFMediaType         **ppType
    )
{
    if (ppType == NULL)
    {
        return E_POINTER;
    }

    GUID    subtype = MFVideoFormat_Base;
    LONG    lStride = 0;
    UINT    cbImage = 0;

    IMFMediaType *pType = NULL;

    // Set the subtype GUID from the FOURCC or D3DFORMAT value.
    subtype.Data1 = fccFormat;

    HRESULT hr = MFCreateMediaType(&pType);
    if (FAILED(hr))
    {
        goto done;
    }

    hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr))
    {
        goto done;
    }

    hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(hr))
    {
        goto done;
    }

    hr = pType->SetUINT32(MF_MT_INTERLACE_MODE, interlaceMode);
    if (FAILED(hr))
    {
        goto done;
    }

    hr = MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, width, height);
    if (FAILED(hr))
    {
        goto done;
    }

    // Calculate the default stride value.
    hr = pType->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(lStride));
    if (FAILED(hr))
    {
        goto done;
    }

    // Calculate the image size in bytes.
    hr = MFCalculateImageSize(subtype, width, height, &cbImage);
    if (FAILED(hr))
    {
        goto done;
    }

    hr = pType->SetUINT32(MF_MT_SAMPLE_SIZE, cbImage);
    if (FAILED(hr))
    {
        goto done;
    }

    hr = pType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (FAILED(hr))
    {
        goto done;
    }

    hr = pType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr))
    {
        goto done;
    }

    // Frame rate
    hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, frameRate.Numerator, 
        frameRate.Denominator);
    if (FAILED(hr))
    {
        goto done;
    }

    // Pixel aspect ratio
    hr = MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, par.Numerator, 
        par.Denominator);
    if (FAILED(hr))
    {
        goto done;
    }

    // Return the pointer to the caller.
    *ppType = pType;
    (*ppType)->AddRef();

done:
    SAFE_RELEASE(pType);
    return hr;
}