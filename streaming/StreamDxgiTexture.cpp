#include "StreamDxgiTexture.h"
#include "SourceDesktop.h"
#include <iostream>
#include <mfapi.h>
#include <Mferror.h>

#pragma comment(lib, "mfplat")

HRESULT StreamDxgiTexture::CheckShutdown() const
{
    return this->source->CheckShutdown();
}

StreamDxgiTexture::StreamDxgiTexture(SourceDesktop* source, 
    IMFStreamDescriptor* descriptor, HRESULT& hr) :
    event_queue(NULL),
    source(source),
    descriptor(descriptor)
{
    assert(source != NULL);
    assert(descriptor != NULL);

    hr = MFCreateEventQueue(&this->event_queue);
}

StreamDxgiTexture::~StreamDxgiTexture()
{
    assert(this->source->state == SourceDesktop::SHUTDOWN_STATE);
}

HRESULT StreamDxgiTexture::QueryInterface(REFIID riid, void** ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(StreamDxgiTexture, IMFMediaStream),
        QITABENT(StreamDxgiTexture, IMFMediaEventGenerator),
        {0}
    };
    return QISearch(this, qit, riid, ppv);
}

HRESULT StreamDxgiTexture::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    HRESULT hr = S_OK; 
    CHECK_HR(hr = this->CheckShutdown()); 
    CHECK_HR(hr = this->event_queue->GetEvent(dwFlags, ppEvent)); 
done: 
    return hr;
}

HRESULT StreamDxgiTexture::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    HRESULT hr = S_OK; 
    CHECK_HR(hr = this->CheckShutdown()); 
    CHECK_HR(hr = this->event_queue->BeginGetEvent(pCallback, punkState)); 
done: 
    return hr;
}

HRESULT StreamDxgiTexture::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    HRESULT hr = S_OK; 
    CHECK_HR(hr = this->CheckShutdown()); 
    CHECK_HR(hr = this->event_queue->EndGetEvent(pResult, ppEvent)); 
done: 
    return hr;
}

HRESULT StreamDxgiTexture::QueueEvent(MediaEventType met, REFGUID guidExtendedType, 
    HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    HRESULT hr = S_OK; 
    CHECK_HR(hr = this->CheckShutdown()); 
    CHECK_HR(hr = this->event_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue)); 
done: 
    return hr;
}

HRESULT StreamDxgiTexture::GetMediaSource(IMFMediaSource** ppMediaSource)
{
    if(ppMediaSource == NULL)
        return E_POINTER;
    
    HRESULT hr = S_OK;
    CHECK_HR(hr = this->CheckShutdown());
    CHECK_HR(hr = this->source->QueryInterface(IID_PPV_ARGS(ppMediaSource)));
done:
    return hr;
}

HRESULT StreamDxgiTexture::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
    if(ppStreamDescriptor == NULL)
        return E_POINTER;

    if(this->descriptor == NULL)
        return E_UNEXPECTED;

    HRESULT hr = this->CheckShutdown();
    if(SUCCEEDED(hr))
    {
        *ppStreamDescriptor = this->descriptor;
        (*ppStreamDescriptor)->AddRef();
    }

    return hr;
}

HRESULT StreamDxgiTexture::RequestSample(IUnknown* pToken)
{
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms700134(v=vs.85).aspx

    HRESULT hr = S_OK;
    CHECK_HR(hr = this->CheckShutdown());

    if(this->source->state == SourceDesktop::STOPPED)
        // apparently MF_E_INVALIDREQUEST is wrong return according to docs;
        // TODO: should be MF_E_MEDIA_SOURCE_WRONGSTATE
        CHECK_HR(hr = MF_E_INVALIDREQUEST);

    this->requests_mutex.Lock();
    this->requests.push(pToken);
    this->requests_mutex.Unlock();

    CHECK_HR(hr = this->DispatchSamples());

done:
    if(FAILED(hr) && this->source->state != SourceDesktop::SHUTDOWN_STATE)
        hr = this->source->QueueEvent(MEError, GUID_NULL, hr, NULL);

    return hr;
}

HRESULT StreamDxgiTexture::DispatchSamples()
{
    HRESULT hr = S_OK;

    // we should not deliver any samples unless the source is running
    if(this->source->state != SourceDesktop::STARTED)
    {
        hr = S_OK;
        goto done;
    }

    {
        ScopedLock samples_lock(this->samples_mutex), requests_lock(this->requests_mutex);

        // deliver as many samples as we can
        while(!this->samples.empty() && !this->requests.empty())
        {
            CComPtr<IMFSample> sample;
            CComPtr<IUnknown> token;

            sample = this->samples.front();
            this->samples.pop();
            token = this->requests.front();
            this->requests.pop();

            if(token)
                CHECK_HR(hr = sample->SetUnknown(MFSampleExtension_Token, token))

            CHECK_HR(hr = 
                this->event_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample));
        }

        // source pushes the samples, no need for the stream to pull

        //if(this->samples.size() < SAMPLE_QUEUE)
        //{
        //    // sample queue is empty and the request queue is not empty, 
        //    // so ask the source for more data
        //    SourceDesktop::Operation op;
        //    op.op = SourceDesktop::REQUEST_SAMPLE;
        //    CHECK_HR(hr = this->source->QueueAsyncOperation(op))
        //}
    }

done:
    if(FAILED(hr) && this->source->state != SourceDesktop::SHUTDOWN_STATE)
        this->source->QueueEvent(MEError, GUID_NULL, hr, NULL);

    return S_OK;
}

HRESULT StreamDxgiTexture::DeliverPayload(IMFSample* sample)
{
    // TODO: remove samples queue since there can be only
    // 1 sample available
    this->samples_mutex.Lock();
    this->samples.push(sample);

    this->samples_mutex.Unlock();

    return this->DispatchSamples();
}

HRESULT StreamDxgiTexture::Shutdown()
{
    ScopedLock samples_lock(this->samples_mutex);
    ScopedLock requests_lock(this->requests_mutex);

    HRESULT hr = S_OK;
    CHECK_HR(hr = this->CheckShutdown());

    // shutdown the event queue
    if(this->event_queue)
        this->event_queue->Shutdown();

    // release objects
    {
        requests_t().swap(this->requests);
        samples_t().swap(this->samples);
    }

done:
    return hr;
}

HRESULT StreamDxgiTexture::Stop()
{
    ScopedLock samples_lock(this->samples_mutex);
    ScopedLock requests_lock(this->requests_mutex);

    HRESULT hr = S_OK;

    CHECK_HR(hr = this->CheckShutdown());

    // clear
    {
        requests_t().swap(this->requests);
        samples_t().swap(this->samples);
    }

    CHECK_HR(hr = this->QueueEvent(MEStreamStopped, GUID_NULL, S_OK, NULL));

done:
    return hr;
}