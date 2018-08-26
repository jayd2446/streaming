#pragma once

#include <Windows.h>
#include <mfidl.h>
#include "assert.h"

namespace tp
{

// fibers are used for locks
class spin_mutex
{

};

class recursive_mutex
{

};

void init_thread_pool();
void shutdown_thread_pool();

typedef void MFWORKITEM_KEY;

HRESULT put_work_item(DWORD queue, IMFAsyncCallback* callback, IUnknown* state);
HRESULT put_waiting_work_item(HANDLE event, LONG priority, IMFAsyncResult* res, MFWORKITEM_KEY* key);
HRESULT schedule_work_item(IMFAsyncCallback* callback, IUnknown* state, INT64 timeout, MFWORKITEM_KEY* key);

}