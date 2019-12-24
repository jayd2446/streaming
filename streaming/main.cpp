#include <iostream>
#include <WinSock2.h>
#include <Windows.h>
#include <mfapi.h>
#include <d3d11.h>
#include "gui_mainwnd.h"
#include <mutex>

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "D3D11.lib")

#define WINDOW_WIDTH 750
#define WINDOW_HEIGHT 700
#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

// defined in project settings
//#define WORKER_STREAMS 3

// TODO: work queue dispatching should be used when the thread might enter a waiting state
// (gpu dispatching etc)
// (actually, the work queue should be used whenever)

// TODO: drop packets in mpeg sink
// TODO: the worker thread size should equal at least to the amount of input samples
// the encoder accepts before outputting samples
// TODO: change fatal error enum to topology switch
// TODO: subsequent request&give calls cannot fail
// TODO: make another function for request_sample thats coming from sink
// TODO: reuse streams from worker stream to reduce memory load on gpu

/*
TODO: mfshutdown should be called only after all other threads have terminated
*/

/*

color converter should be used only if needed;
it seems that the amd h264 encoder creates minimal artifacts if the color converter is used

*/

/*

streams are single threaded; components are multithreaded
streams take d3d context as constructor parameter

one topology is enough for the multithreading

direct3d context should be restricted to topology level;
d3d11 device is multithread safe

decide if concurrency is implemented in topology granularity instead;
work queues further reduce the granularity to subprocedure level

this would need a clock that is shared between topologies;
(that means an implementation of time source object)

also some components should also be shared between topologies

*/

CAppModule module_;

#ifdef _DEBUG

// librtmp debug prints
extern "C" FILE* netstackdump = nullptr;
extern "C" FILE* netstackdump_read = nullptr;

int YourReportHook( int reportType, char *message, int *returnValue )
{
    OutputDebugStringA(message);
    if(reportType == _CRT_ASSERT)
        DebugBreak();
    *returnValue = TRUE;
    return TRUE;
}

#endif

//DWORD capture_work_queue_id;
// greater priority value has a greater priority
//LONG capture_audio_priority = 10;

int main()
{
    try
    {
        HRESULT hr = S_OK;
        WSADATA wsa_data = {0};

        CHECK_HR(hr = CoInitializeEx(NULL, COINIT_SPEED_OVER_MEMORY | COINIT_MULTITHREADED));
        AtlInitCommonControls(ICC_COOL_CLASSES | ICC_BAR_CLASSES | ICC_WIN95_CLASSES);

        const int wsa_init_res = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        if(wsa_init_res != 0)
            throw HR_EXCEPTION(E_UNEXPECTED);

        CHECK_HR(hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
        CHECK_HR(hr = module_.Init(NULL, NULL));

        // lock a capture priority multithreaded work queue
        /*DWORD task_id = 0;
        CHECK_HR(hr = MFLockSharedWorkQueue(L"Capture", 0, &task_id, &capture_work_queue_id));*/

        //// register all media foundation standard work queues as playback
        /*DWORD taskgroup_id = 0;
        if(FAILED(MFRegisterPlatformWithMMCSS(L"Capture", &taskgroup_id, AVRT_PRIORITY_NORMAL)))
            throw HR_EXCEPTION(hr);*/

#ifdef _DEBUG
        fopen_s(&netstackdump, "nul", "w");
        fopen_s(&netstackdump_read, "nul", "w");

        // make atl/wtl asserts to break immediately
        _CrtSetReportHook(YourReportHook);
#endif

        {
            CMessageLoop msgloop;
            module_.AddMessageLoop(&msgloop);
            {
                /*gui_frame wnd(module_);*/
                gui_mainwnd wnd;
                RECT r = {CW_USEDEFAULT, CW_USEDEFAULT,
                    CW_USEDEFAULT + WINDOW_WIDTH, CW_USEDEFAULT + WINDOW_HEIGHT};
                if(wnd.CreateEx(NULL, &r) == NULL)
                    throw HR_EXCEPTION(E_UNEXPECTED);
                wnd.ShowWindow(SW_SHOW);
                wnd.UpdateWindow();
                int ret = msgloop.Run();
                module_.RemoveMessageLoop();
                ret;
            }
        }

done:
        if(FAILED(hr))
            throw HR_EXCEPTION(hr);

        // unlocking the work queue might crash ongoing async operations,
        // so it is safer just to call mfshutdown
        /*hr = MFUnlockWorkQueue(capture_work_queue_id);*/

#ifdef _DEBUG
        WSACleanup();
        module_.Term();
        hr = MFShutdown();
        CoUninitialize();
#endif

    }
    catch(streaming::exception e)
    {
        streaming::print_error_and_abort(e.what());
    }


    return 0;
}