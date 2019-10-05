#pragma once
#include "async_callback.h"
#include "request_packet.h"
#include "buffer_pool.h"
#include <functional>
#include <memory>
#include <mfapi.h>

// helper class for dispatching multiple requests as work items;
// the last queued request can be served without the dispatcher

// request dispatcher is just a mf put work item wrapper with arguments

template<class Request>
class request_dispatcher final : public enable_shared_from_this
{
public:
    struct state_object;
    typedef async_callback<request_dispatcher> async_callback_t;
    typedef Request request_t;
    using state_object_t = std::shared_ptr <state_object>;
    using state_object_pooled = buffer_pooled<state_object>;
    using buffer_pool_state_object_t = buffer_pool<state_object_pooled>;
private:
    CComPtr<async_callback_t> dispatch_callback;
    std::shared_ptr<buffer_pool_state_object_t> buffer_pool_state_object;
    void dispatch_cb(void*);
public:
    request_dispatcher();
    ~request_dispatcher();
    void dispatch_request(request_t&&, std::function<void(request_t&)>);
};


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

template<class T>
struct request_dispatcher<T>::state_object : public buffer_poolable,
    IUnknown, IUnknownImpl
{
private:
    bool at_destructor;
public:
    state_object_t this_;
    std::function<void(request_t&)> on_dispatch;
    request_t request;

    state_object() : at_destructor(false) {}
    ~state_object()
    {
        this->at_destructor = true;
        this->Release();
    }

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

    void release_this() override
    {
        if(this->at_destructor)
            return;

        this->AddRef();

        // custom deleter of this_ either deletes this or moves it back to the pool
        this->this_ = nullptr;
    }

    void initialize(const state_object_t& this_)
    {
        this->buffer_poolable::initialize();
        this->this_ = this_;
    }

    void uninitialize() override 
    {
        this->buffer_poolable::uninitialize();

        this->on_dispatch = std::function<void(request_t&)>();
        this->request = request_t();
    }
};

template<class T>
request_dispatcher<T>::request_dispatcher() : 
    buffer_pool_state_object(new buffer_pool_state_object_t)
{
    this->dispatch_callback.Attach(new async_callback_t(&request_dispatcher::dispatch_cb));
}

template<class T>
request_dispatcher<T>::~request_dispatcher()
{
    buffer_pool_state_object_t::scoped_lock lock(this->buffer_pool_state_object->mutex);
    this->buffer_pool_state_object->dispose();
}

template<class T>
void request_dispatcher<T>::dispatch_cb(void* res_)
{
    // TODO: make sure that the imfasyncresult isn't kept alive for longer than the duration of
    // dispatch_cb

    assert_(res_);
    IMFAsyncResult* res = static_cast<IMFAsyncResult*>(res_);
    CComPtr<IUnknown> params_unk;
    state_object* params;

    HRESULT hr = S_OK;
    CHECK_HR(hr = res->GetState(&params_unk));

    params = static_cast<state_object*>(params_unk.p);
    params->on_dispatch(params->request);

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

template<class T>
void request_dispatcher<T>::dispatch_request(request_t&& request, std::function<void(request_t&)> f)
{
    HRESULT hr = S_OK;

    CComPtr<state_object> params;
    {
        buffer_pool_state_object_t::scoped_lock lock(this->buffer_pool_state_object->mutex);
        state_object_t state_object = this->buffer_pool_state_object->acquire_buffer();
        state_object->initialize(state_object);

        params.Attach(state_object.get());
    }

    params->request = std::move(request);
    params->on_dispatch = std::move(f);

    CHECK_HR(hr = this->dispatch_callback->mf_put_work_item(
        this->shared_from_this<request_dispatcher>(), params.p));

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
}

#undef CHECK_HR