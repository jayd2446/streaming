#pragma once

#include <queue>
#include <mfidl.h>
#include "common_atl.h"

#pragma comment(lib, "mfuuid")      // Media Foundation GUIDs

#define SAMPLE_QUEUE 0 // 2

class SourceDesktop;

class StreamDxgiTexture : IUnknownImpl, public IMFMediaStream
{
public:
    typedef std::queue<CComPtr<IUnknown>> requests_t;
    typedef std::queue<CComPtr<IMFSample>> samples_t;
private:
    CComPtr<IMFMediaEventQueue> event_queue; // event generator helper
    CComPtr<SourceDesktop> source;
    CComPtr<IMFStreamDescriptor> descriptor;

    requests_t requests;
    samples_t samples;

    CComAutoCriticalSection requests_mutex;
    CComAutoCriticalSection samples_mutex;

    HRESULT CheckShutdown() const;
public:
    StreamDxgiTexture(SourceDesktop*, IMFStreamDescriptor*, HRESULT&);
    ~StreamDxgiTexture();

    // sourcedesktop calls this
    HRESULT Stop();

    // delivers all outstanding requests
    HRESULT DispatchSamples();
    // adds sample to the sample queue
    HRESULT DeliverPayload(IMFSample*);
    // releases the resources used by this
    HRESULT Shutdown();

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() {return IUnknownImpl::AddRef();}
    ULONG STDMETHODCALLTYPE Release() {return IUnknownImpl::Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv);

    // IMFMediaEventGenerator
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent);
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState);
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent);
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, 
        const PROPVARIANT* pvValue);

    // IMFMediaStream
    HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource** ppMediaSource);
    HRESULT STDMETHODCALLTYPE GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor);
    HRESULT STDMETHODCALLTYPE RequestSample(IUnknown* pToken);
};