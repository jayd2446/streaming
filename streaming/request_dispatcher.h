#pragma once
#include "async_callback.h"
#include "request_packet.h"
#include <functional>
#include <memory>
#include <mfapi.h>

// helper class for dispatching multiple requests as work items;
// the last queued request can be served without the dispatcher

// request dispatcher is just a mf put work item wrapper with arguments

template<class Request>
class request_dispatcher : public enable_shared_from_this
{
public:
    struct state_object;
    typedef async_callback<request_dispatcher> async_callback_t;
    typedef Request request_t;
private:
    CComPtr<async_callback_t> dispatch_callback;
    void dispatch_cb(void*);
public:
    request_dispatcher();
    void dispatch_request(request_t&&, std::function<void(request_t&)>);
};


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

template<class T>
struct request_dispatcher<T>::state_object : IUnknown, IUnknownImpl
{
    std::function<void(request_t&)> on_dispatch;
    request_t request;

    ULONG STDMETHODCALLTYPE AddRef() {return IUnknownImpl::AddRef();}
    ULONG STDMETHODCALLTYPE Release() {return IUnknownImpl::Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv)
    {
        if(!ppv)
            return E_POINTER;
        if(riid == __uuidof(IUnknown))
            *ppv = static_cast<IUnknown*>(this);
        else
        {
            *ppv = NULL;
            return E_NOINTERFACE;
        }

        this->AddRef();
        return S_OK;
    }
};

template<class T>
request_dispatcher<T>::request_dispatcher()
{
    this->dispatch_callback.Attach(new async_callback_t(&request_dispatcher::dispatch_cb));
}

template<class T>
void request_dispatcher<T>::dispatch_cb(void* res_)
{
    assert_(res_);
    IMFAsyncResult* res = static_cast<IMFAsyncResult*>(res_);
    CComPtr<state_object> params;

    HRESULT hr = S_OK;
    CHECK_HR(hr = res->GetState((IUnknown**)&params));

    params->on_dispatch(params->request);

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

template<class T>
void request_dispatcher<T>::dispatch_request(request_t&& request, std::function<void(request_t&)> f)
{
    HRESULT hr = S_OK;

    // TODO: pool state objects
    CComPtr<state_object> params;
    params.Attach(new state_object);
    params->request = std::move(request);
    params->on_dispatch = std::move(f);

    CHECK_HR(hr = this->dispatch_callback->mf_put_work_item(
        this->shared_from_this<request_dispatcher>(), params.p));

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
}

#undef CHECK_HR