#pragma once

#include "AsyncCallback.h"
#include "IUnknownImpl.h"
#include <mfapi.h>
#include <atlbase.h>
#include <memory>
#include <iostream>
#include "assert.h"

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
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    std::mutex mutex;
    std::weak_ptr<T> parent;
    invoke_fn cb;

    HRESULT mf_cb(IMFAsyncResult* res)
    {
        std::shared_ptr<T> parent;
        {
            scoped_lock lock(this->mutex);
            parent = this->parent.lock();
        }

        if(parent)
        {
            // wait until the error is processed
            streaming::check_for_errors();

            try
            {
                (parent.get()->*cb)((void*)res);
            }
            catch(streaming::exception e)
            {
                streaming::print_error_and_abort(e.what());
            }
        }
        else
            // this assert_ might fail when the parent is currently @ the destructor;
            // the locking of the parent isn't possible anymore, but
            // the parent has still a reference to this in addition to the
            // media foundation callback having a reference to this
            maybe_assert(this->RefCount == 1);
            /*assert_(this->RefCount == 1);*/
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

    void set_callback(const std::weak_ptr<T>& parent) 
    {
        scoped_lock lock(this->mutex);
        this->parent = parent;
    }
    void set_callback(
        const std::weak_ptr<T>& parent,
        invoke_fn cb) {this->set_callback(parent); std::atomic_exchange(&this->cb, cb);}

    void invoke(void* unk) {this->mf_cb((IMFAsyncResult*)unk);}

    HRESULT mf_put_work_item()
    {
        return MFPutWorkItem(this->native.work_queue, &this->native, NULL);
    }
    // the parent must be same for each call
    HRESULT mf_put_work_item(const std::weak_ptr<T>& parent, IUnknown* state = NULL)
    {
        this->set_callback(parent);
        return MFPutWorkItem(this->native.work_queue, &this->native, state);
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
    HRESULT mf_schedule_work_item(
        const std::weak_ptr<T>& parent,
        INT64 timeout_ms,
        MFWORKITEM_KEY* key)
    {
        this->set_callback(parent);
        return MFScheduleWorkItem(&this->native, NULL, -timeout_ms, key);
    }
};