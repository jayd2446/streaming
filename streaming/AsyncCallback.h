#pragma once

#include <mfidl.h>

#pragma comment(lib, "Mfuuid.lib")

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