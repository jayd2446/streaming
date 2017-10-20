#pragma once

#include "AsyncCallback.h"
#include "IUnknownImpl.h"
#include <mfapi.h>
#include <atlbase.h>
#include <memory>
#include <cassert>
#include <atomic>

#pragma comment(lib, "Mfplat.lib")

// wrapper for the com async callback

// async_callback must be wrapped inside ccomptr;
// ccomptr must be attached by Attach() call,
// otherwise memory leak occurs
template<class T>
class async_callback : public IUnknownImpl
{
public:
    typedef void (T::*invoke_fn)(void*);
    typedef T parent_t;
private:
    std::weak_ptr<T> parent;
    invoke_fn cb;

    HRESULT mf_cb(IMFAsyncResult* res)
    {
        const std::shared_ptr<T> parent = this->parent.lock();
        if(parent)
            (parent.get()->*cb)((void*)res);
        else
            // this assert might fail when the parent is currently @ the destructor;
            // the locking of the parent isn't possible anymore, but
            // the parent has still a reference to this in addition to the
            // media foundation callback having a reference to this
            assert(this->RefCount == 1);
        return S_OK;
    }
public:
    AsyncCallback<async_callback<T>> native;

    async_callback(
        invoke_fn cb,
        DWORD work_queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED,
        DWORD flags = 0) : 
        parent(parent), 
        cb(cb), 
        native(this, &async_callback<T>::mf_cb, work_queue, flags) {}

    // TODO: these should be atomic operations
    void set_callback(const std::weak_ptr<T>& parent) {this->parent = parent;}
    void set_callback(
        const std::weak_ptr<T>& parent,
        invoke_fn cb) {this->set_callback(parent); std::atomic_exchange(&this->cb, cb);}

    void invoke(void* unk) {this->mf_cb((IMFAsyncResult*)unk);}

    // the parent must be same for each call
    HRESULT mf_put_work_item(const std::weak_ptr<T>& parent, DWORD queue) 
    {
        this->set_callback(parent);
        return MFPutWorkItem(queue, &this->native, NULL);
    }
    HRESULT mf_put_waiting_work_item(
        const std::weak_ptr<T>& parent, 
        HANDLE hEvent,
        LONG priority,
        IMFAsyncResult* result,
        MFWORKITEM_KEY* key)
    {
        this->set_callback(parent);
        return MFPutWaitingWorkItem(hEvent, priority, result, key);
    }
};