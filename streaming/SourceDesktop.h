#pragma once

#include <queue>
#include <mfapi.h>
#include <mfidl.h>
#include <Mferror.h>
#include <dxgi1_2.h>
#include "common_atl.h"

#pragma comment(lib, "dxgi")

// https://msdn.microsoft.com/en-us/library/windows/desktop/ff485865(v=vs.85).aspx
HRESULT CreateUncompressedVideoType(
    DWORD                fccFormat,  // FOURCC or D3DFORMAT value.     
    UINT32               width, 
    UINT32               height,
    MFVideoInterlaceMode interlaceMode,
    const MFRatio&       frameRate,
    const MFRatio&       par,
    IMFMediaType         **ppType
    );

// async callback wrapper for class member function
template<class T>
class AsyncCallback : public IMFAsyncCallback
{
public:
    typedef HRESULT (T::*InvokeFn)(IMFAsyncResult *pAsyncResult);
private:
    T* parent;
    InvokeFn Fn;
public:
    AsyncCallback(T* parent, InvokeFn Fn) : parent(parent), Fn(Fn) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() {return parent->AddRef();}
    ULONG STDMETHODCALLTYPE Release() {return parent->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv)
    {
        if(!ppv)
            return E_POINTER;
        if(riid == __uuidof(IUnknown))
            *ppv = static_cast<IUnknown*>(static_cast<IMFAsyncCallback*>(this));
        else if(riid == __uuidof(IMFAsyncCallback))
            *ppv = static_cast<IMFAsyncCallback*>(this);
        else
        {
            *ppv = NULL;
            return E_NOINTERFACE;
        }

        this->AddRef();
        return S_OK;
    }

    // IMFAsyncCallback
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD*, DWORD*) {return E_NOTIMPL;} // optional
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) {return (this->parent->*Fn)(result);}
};

class StreamDxgiTexture;

// TODO: push samples in an fps limited manner

class SourceDesktop : IUnknownImpl, public IMFMediaSourceEx, public IMFRealTimeClientEx
{
public:
    static HRESULT CreateInstance(SourceDesktop**);

    enum State_t
    {
        STARTED,
        PAUSED,
        STOPPED,
        SHUTDOWN_STATE,
        INVALID
    };
    enum Operation_t
    {
        START,
        PAUSE,
        STOP
    };
    struct Operation
    {
        Operation_t op;
        // should be set when starting the stream
        CComPtr<IMFPresentationDescriptor> pd;
        // should be set when starting the stream
        PROPVARIANT startpos;
    };
    typedef std::queue<Operation> op_queue_t;
private:
    CComPtr<IMFPresentationDescriptor> descriptor;
    CComPtr<IMFMediaEventQueue> event_queue; // event generator helper
    CComPtr<StreamDxgiTexture> stream;
    AsyncCallback<SourceDesktop> OnProcessQueue_cb;
    CComAutoCriticalSection op_queue_mutex;
    CComAutoCriticalSection op_mutex;
    op_queue_t op_queue;
    CComPtr<IMFAttributes> attributes;

    AsyncCallback<SourceDesktop> OnRequestSample_cb;
    MFWORKITEM_KEY schedule_item_key;

    CComPtr<IMFDXGIDeviceManager> device_manager;
    CComPtr<IDXGIOutputDuplication> screen_capture;
    bool screen_frame_released;
    LARGE_INTEGER start_time;
    AsyncCallback<SourceDesktop> OnSampleReleased_cb;

    DWORD dwMultithreadedWorkQueueId;
    LONG  lWorkItemBasePriority;

    HRESULT ScheduleSampleSubmit(bool cancel);
    // streams will call this to request samples
    HRESULT QueueAsyncOperation(Operation& op);
    // invokes the next operation on the worker thread
    HRESULT ProcessQueue();
    // this is called from the worker thread;
    // it is synchronized so that only one operation will be active at a time
    HRESULT ProcessQueueAsync(IMFAsyncResult* pResult);
    void OnAsyncOp(Operation& op);

    // initializes the source
    HRESULT Initialize();
    HRESULT InitializeScreenCapture();

    // async operations;
    // these will signal results via event_queue
    HRESULT OnPause(Operation& op);
    HRESULT OnStop(Operation& op);
    HRESULT OnStart(Operation& op);
    HRESULT OnRequestSample(IMFAsyncResult* pResult);

    // called when the pipeline has released the pushed sample
    HRESULT OnSampleReleased(IMFAsyncResult* pResult);

    explicit SourceDesktop(HRESULT&);
public:
    volatile State_t state;

    ~SourceDesktop();

    HRESULT CheckShutdown() const {return this->state == SHUTDOWN_STATE ? MF_E_SHUTDOWN : S_OK;}
    HRESULT CheckInitialized() const {return this->state == INVALID ? MF_E_NOT_INITIALIZED : S_OK;}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() {return IUnknownImpl::AddRef();}
    ULONG STDMETHODCALLTYPE Release() {return IUnknownImpl::Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv);

    // IMFMediaSource
    HRESULT STDMETHODCALLTYPE CreatePresentationDescriptor(IMFPresentationDescriptor**);
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* pdwCharacteristics);
    HRESULT STDMETHODCALLTYPE Pause() {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Shutdown();
    HRESULT STDMETHODCALLTYPE Start(
        IMFPresentationDescriptor* pPresentationDescriptor,
        const GUID* pguidTimeFormat,
        const PROPVARIANT* pvarStartPosition);
    HRESULT STDMETHODCALLTYPE Stop();

    // IMFMediaSourceEx
    HRESULT STDMETHODCALLTYPE GetSourceAttributes(IMFAttributes**);
    HRESULT STDMETHODCALLTYPE GetStreamAttributes(DWORD, IMFAttributes**);
    HRESULT STDMETHODCALLTYPE SetD3DManager(IUnknown*);

    // IMFRealTimeClientEx
    // this interface allows use of mmcss for work queues this object uses
    HRESULT STDMETHODCALLTYPE RegisterThreadsEx(DWORD*, LPCWSTR, LONG) {return S_OK;}
    HRESULT STDMETHODCALLTYPE SetWorkQueueEx(DWORD a, LONG b) 
    {this->dwMultithreadedWorkQueueId = a; this->lWorkItemBasePriority = b; return S_OK;}
    HRESULT STDMETHODCALLTYPE UnregisterThreads() {return S_OK;}

    // IMFMediaEventGenerator
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent);
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState);
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent);
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, 
        const PROPVARIANT* pvValue);
};