#pragma once

#include <atlbase.h>
#include <assert.h>

#undef assert
#define assert(b) {if(!(b)) DebugBreak();}

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#define SAFE_RELEASE(x) { if (x) { x->Release(); x = NULL; } }

typedef CComCritSecLock<CComAutoCriticalSection> ScopedLock;

// performance counter frequency
extern LARGE_INTEGER pc_frequency;

class IUnknownImpl
{
protected:
    volatile long RefCount;
public:
    IUnknownImpl() : RefCount(1) {}
    virtual ~IUnknownImpl() {assert(this->RefCount == 0);}

    ULONG AddRef() {return InterlockedIncrement(&this->RefCount);}
    ULONG Release()
    {
        assert(this->RefCount > 0);
        ULONG count = InterlockedDecrement(&this->RefCount);
        if(count == 0)
            delete this;
        return count;
    }
};