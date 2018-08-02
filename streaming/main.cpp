#include <iostream>
#include <Windows.h>
#include <mfapi.h>
#include <d3d11.h>
//#include "source_displaycapture.h"
//#include "source_displaycapture2.h"
//#include "source_displaycapture3.h"
//#include "source_displaycapture4.h"
//#include "source_displaycapture5.h"
//#include "sink_preview.h"
//#include "media_session.h"
//#include "media_topology.h"
//#include "transform_videoprocessor.h"
//#include "transform_h264_encoder.h"
//#include "transform_color_converter.h"
//#include "sink_mpeg2.h"
//#include "sink_preview2.h"
//#include "source_loopback.h"
//#include "transform_aac_encoder.h"
//#include "transform_audiomix.h"
//#include "transform_audioprocessor.h"
#include "gui_frame.h"
#include <mutex>

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "D3D11.lib")

LARGE_INTEGER pc_frequency;

#define WINDOW_WIDTH 750
#define WINDOW_HEIGHT 700
#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

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

#ifdef _DEBUG
int YourReportHook( int reportType, char *message, int *returnValue )
{
    OutputDebugStringA(message);
    if(reportType == _CRT_ASSERT)
        DebugBreak();
    *returnValue = TRUE;
    return TRUE;
}
#endif

int main()
{
    QueryPerformanceFrequency(&pc_frequency);

    CAppModule module;

    HRESULT hr = S_OK;
    CHECK_HR(hr = CoInitializeEx(NULL, COINIT_SPEED_OVER_MEMORY | COINIT_MULTITHREADED));
    CHECK_HR(hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    CHECK_HR(hr = module.Init(NULL, NULL));

    //// register all media foundation standard work queues as playback
    /*DWORD taskgroup_id = 0;
    if(FAILED(MFRegisterPlatformWithMMCSS(L"Capture", &taskgroup_id, AVRT_PRIORITY_NORMAL)))
        throw std::exception();*/

    // make atl/wtl asserts to break immediately
#ifdef _DEBUG
    _CrtSetReportHook(YourReportHook);
#endif

    {
        CMessageLoop msgloop;
        module.AddMessageLoop(&msgloop);
        {
            gui_frame wnd(module);
            RECT r = {CW_USEDEFAULT, CW_USEDEFAULT, 
                CW_USEDEFAULT + WINDOW_WIDTH, CW_USEDEFAULT + WINDOW_HEIGHT};
            if(wnd.CreateEx(NULL, &r) == NULL)
                throw std::exception();
            wnd.ShowWindow(SW_SHOW);
            wnd.UpdateWindow();
            int ret = msgloop.Run();
            module.RemoveMessageLoop();
            ret;
        }
    }

done:
    if(FAILED(hr))
        throw std::exception();
    module.Term();
    hr = MFShutdown();
    CoUninitialize();

    return 0;
}