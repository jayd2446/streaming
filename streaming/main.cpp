#include <iostream>
#include <Windows.h>
#include <mfapi.h>
#include <d3d11.h>
//#include "source_displaycapture.h"
//#include "source_displaycapture2.h"
//#include "source_displaycapture3.h"
#include "source_displaycapture4.h"
#include "sink_preview.h"
#include "media_session.h"
#include "media_topology.h"
#include "transform_videoprocessor.h"
//#include "transform_h264_encoder.h"
//#include "transform_color_converter.h"

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "D3D11.lib")

LARGE_INTEGER pc_frequency;

#define WINDOW_WIDTH 1200
#define WINDOW_HEIGHT 800
#define OUTPUT_MONITOR 0
#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
HWND create_window();

void create_streams(
    ID3D11DeviceContext* devctx,
    const media_topology_t& topology,
    const sink_preview_t& preview_sink,
    const source_displaycapture4_t& displaycapture_source2,
    const source_displaycapture4_t& displaycapture_source,
    const transform_videoprocessor_t& videoprocessor_transform)
{
    HRESULT hr = S_OK;

    // (each thread gets approximately 20ms time slice)
    // TODO: decide if use work queues for streams in a same node
    // TODO: sinks should also have streams for each core
    media_stream_t sink_stream = preview_sink->create_stream(topology->get_clock());

    for(int i = 0; i < QUEUE_MAX_SIZE; i++)
    {
        media_stream_t transform_stream = videoprocessor_transform->create_stream(devctx);
        media_stream_t source_stream = displaycapture_source->create_stream();
        media_stream_t source_stream2 = displaycapture_source2->create_stream();
        preview_sink->concurrent_streams[i] = transform_stream;
        preview_sink->pending_streams[i].available = true;

        topology->connect_streams(source_stream, preview_sink->concurrent_streams[i]);
        topology->connect_streams(source_stream2, preview_sink->concurrent_streams[i]);
        topology->connect_streams(preview_sink->concurrent_streams[i], sink_stream);
    }

done:
    if(FAILED(hr))
        throw std::exception();

    /*media_stream_t color_converter_stream = color_converter_transform->create_stream();*/
    /*media_stream_t h264_encoder_stream = h264_encoder_transform->create_stream();*/
    /*bool b = true;
    b &= topology->connect_streams(source_stream, transform_stream);
    b &= topology->connect_streams(source_stream2, transform_stream);
    b &= topology->connect_streams(transform_stream, sink_stream);*/
    /*b &= topology->connect_streams(color_converter_stream, sink_stream);*/
    /*b &= topology->connect_streams(h264_encoder_stream, sink_stream);*/
    /*b &= topology->connect_streams(source_stream, sink_stream);*/
}

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

int main()
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_SPEED_OVER_MEMORY);
    hr = MFStartup(MFSTARTUP_NOSOCKET);

    //// register all media foundation standard work queues as playback
    //DWORD taskgroup_id = 0;
    //if(FAILED(MFRegisterPlatformWithMMCSS(L"Playback", &taskgroup_id, AVRT_PRIORITY_NORMAL)))
    //    throw std::exception();

    HWND hwnd = create_window();

    QueryPerformanceFrequency(&pc_frequency);

    // create window, direct3d 11 device, context and swap chain
    /*HWND hwnd = create_window();*/

    CComPtr<IDXGISwapChain> swapchain;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> d3d11devctx;

    hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION, &d3d11dev, NULL, &d3d11devctx);

    /*Sleep(10000);*/

    /*while(true)*/
    {
        UINT outputmonitor_index = OUTPUT_MONITOR;

        // create the session and the topology
        media_session_t session(new media_session);
        media_topology_t topology(new media_topology);

        //// create and initialize the h264 encoder transform
        //transform_h264_encoder_t h264_encoder_transform(new transform_h264_encoder(session));
        //hr = h264_encoder_transform->initialize(d3d11dev);

        // create and initialize the color converter transform
        /*transform_color_converter_t color_converter_transform(new transform_color_converter(session));
        hr = color_converter_transform->initialize(d3d11dev);*/

        // create and initialize the video processor transform
        transform_videoprocessor_t videoprocessor_transform(new transform_videoprocessor(session));
        hr = videoprocessor_transform->initialize(d3d11dev);

        // create and initialize the display capture source
        source_displaycapture4_t displaycapture_source(new source_displaycapture4(session));
        source_displaycapture4_t displaycapture_source2(new source_displaycapture4(session));
        hr = displaycapture_source->initialize(0, d3d11dev);
        hr |= displaycapture_source2->initialize(1, d3d11dev);
        if(FAILED(hr))
        {
            std::cerr << "could not initialize display capture source" << std::endl;
            system("pause");
            return 0;
        }

        // create and initialize the preview window sink
        sink_preview_t preview_sink(new sink_preview(session));
        preview_sink->initialize(
            WINDOW_WIDTH, WINDOW_HEIGHT,
            hwnd, d3d11dev, d3d11devctx, swapchain);

        // initialize the topology
        create_streams(
            d3d11devctx,
            topology,
            preview_sink,
            displaycapture_source,
            displaycapture_source2,
            videoprocessor_transform);

        // add the topology to the media session
        session->switch_topology(topology);

        // start the media session
        session->start_playback(0);

        bool switched_topology = false;
        MSG msg = {};
        while(GetMessage(&msg, NULL, 0, 0))
        {
            if(msg.message == WM_KEYDOWN)
            {
                outputmonitor_index = (outputmonitor_index + 1) % 2;

                if(outputmonitor_index)
                {
                    topology.reset(new media_topology);
                    create_streams(
                        d3d11devctx,
                        topology,
                        preview_sink,
                        displaycapture_source2,
                        displaycapture_source,
                        videoprocessor_transform);
                }
                else
                {
                    topology.reset(new media_topology);
                    create_streams(
                        d3d11devctx,
                        topology,
                        preview_sink,
                        displaycapture_source,
                        displaycapture_source2,
                        videoprocessor_transform);
                }

                session->switch_topology(topology);

                /*for(int i = 0; i < 25001; i++)*/
                //{
                //    outputmonitor_index = (outputmonitor_index + 1) % 2;

                //    // switch topology
                //    topology.reset(new media_topology);

                //    sink_stream = preview_sink->create_stream(topology->get_clock());
                //    source_stream = displaycapture_source->create_stream();
                //    source_stream2 = displaycapture_source2->create_stream();
                //    transform_stream = videoprocessor_transform->create_stream();
                //    color_converter_stream = color_converter_transform->create_stream();

                //    bool b = true;
                //    if(!outputmonitor_index)
                //    {
                //        b &= topology->connect_streams(source_stream, transform_stream);
                //        b &= topology->connect_streams(source_stream2, transform_stream);
                //    }
                //    else
                //    {
                //        b &= topology->connect_streams(source_stream2, transform_stream);
                //        b &= topology->connect_streams(source_stream, transform_stream);
                //    }
                //    b &= topology->connect_streams(transform_stream, color_converter_stream);
                //    b &= topology->connect_streams(color_converter_stream, sink_stream);
                //    session->switch_topology(topology);
                //}
            }
            else
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            /*Sleep(10000);
            break;*/
        }

        session->stop_playback();

        // clear the references in sink
        for(int i = 0; i < QUEUE_MAX_SIZE; i++)
            preview_sink->concurrent_streams[i] = NULL;

        // shutdown the media session
        session->shutdown();
    }


    /*long c = topology.use_count();
    long cc = topology->clock.use_count();
    c = 0;
    topology = NULL;*/

    // main should wait for all threads to terminate before terminating itself;
    // this sleep fakes that functionality
    Sleep(1000);
    hr = MFShutdown();

    return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch(uMsg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

HWND create_window()
{
    LPCTSTR class_name = L"streaming";

    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = class_name;
    RegisterClass(&wc);

    RECT r;
    r.top = r.left = 0;
    r.right = WINDOW_WIDTH;
    r.bottom = WINDOW_HEIGHT;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowEx(
        0,
        class_name,
        L"output",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right, r.bottom,
        NULL,
        NULL,
        GetModuleHandle(NULL),
        NULL);

    ShowWindow(hwnd, SW_SHOW);

    return hwnd;
}