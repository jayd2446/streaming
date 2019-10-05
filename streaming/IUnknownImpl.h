#pragma once

#include <Windows.h>
#include "assert.h"

class IUnknownImpl
{
protected:
    volatile long RefCount;
    virtual void release_this() { delete this; }
public:
    IUnknownImpl() : RefCount(1) {}
    virtual ~IUnknownImpl() {assert_(this->RefCount == 0);}

    ULONG AddRef() {return InterlockedIncrement(&this->RefCount);}
    ULONG Release()
    {
        assert_(this->RefCount > 0);
        ULONG count = InterlockedDecrement(&this->RefCount);
        if(count == 0)
            this->release_this();
        return count;
    }
};