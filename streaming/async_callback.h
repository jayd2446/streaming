#pragma once

#include "AsyncCallback.h"
#include "IUnknownImpl.h"
#include <mfapi.h>
#include <atlbase.h>
#include <memory>
#include <cassert>

#pragma comment(lib, "Mfplat.lib")

// wrapper for the com async callback

// async_callback must be wrapped inside ccomptr
template<class T>
class async_callback : public IUnknownImpl
{
public:
    typedef void (T::*invoke_fn)();
    typedef T parent_t;
private:
    std::weak_ptr<T> parent;
    invoke_fn cb;

    HRESULT mf_cb(IMFAsyncResult*)
    {
        const std::shared_ptr<T> parent = this->parent.lock();
        if(parent)
            (parent.get()->*cb)();
        return S_OK;
    }
public:
    AsyncCallback<async_callback<T>> native;

    async_callback(
        invoke_fn cb,
        DWORD work_queue = MFASYNC_CALLBACK_QUEUE_STANDARD,
        DWORD flags = 0) : 
        parent(parent), 
        cb(cb), 
        native(this, &async_callback<T>::mf_cb, work_queue, flags) {}

    // the parent must be same for each call
    HRESULT mf_put_work_item(const std::weak_ptr<T>& parent, DWORD queue) 
    {
        /*assert(!this->parent || this->parent == parent);*/
        /*if(!this->parent)*/
        this->parent = parent;
        return MFPutWorkItem(queue, &this->native, NULL);
    }
    HRESULT mf_put_waiting_work_item(
        const std::weak_ptr<T>& parent, 
        HANDLE hEvent,
        LONG priority,
        IMFAsyncResult* result,
        MFWORKITEM_KEY* key)
    {
        /*assert(!this->parent || this->parent == parent);*/
        /*if(!this->parent)*/
        this->parent = parent;
        return MFPutWaitingWorkItem(hEvent, priority, result, key);
    }
};