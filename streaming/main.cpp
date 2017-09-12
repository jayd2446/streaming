#include <iostream>
#include <Windows.h>
#include <mfapi.h>
#include <d3d11.h>
#include "source_displaycapture.h"
#include "source_displaycapture2.h"
#include "sink_preview.h"
#include "media_session.h"
#include "media_topology.h"

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "D3D11.lib")

LARGE_INTEGER pc_frequency;

#define WINDOW_WIDTH 1200
#define WINDOW_HEIGHT 800

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
HWND create_window();

int main()
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_SPEED_OVER_MEMORY);
    hr = MFStartup(MFSTARTUP_NOSOCKET);

    /*Sleep(5000);*/

    /*for(int i = 0; i < (2000 / 50); i)*/
    {
        QueryPerformanceFrequency(&pc_frequency);

        // create window, direct3d 11 device, context and swap chain
        HWND hwnd = create_window();

        CComPtr<IDXGISwapChain> swapchain;
        CComPtr<ID3D11Device> d3d11dev;
        CComPtr<ID3D11DeviceContext> d3d11devctx;

        hr = D3D11CreateDevice(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            NULL, 0, D3D11_SDK_VERSION, &d3d11dev, NULL, &d3d11devctx);

        media_session_t session(new media_session);
        media_topology_t topology(new media_topology);

        // create and initialize the display capture source
        source_displaycapture2_t displaycapture_source(new source_displaycapture2(session));
        hr = displaycapture_source->initialize(d3d11dev, d3d11devctx);
        if(FAILED(hr))
        {
            std::cerr << "could not initialize display capture source" << std::endl;
            system("pause");
            return 0;
        }

        // create and initialize the preview window sink
        sink_preview_t preview_sink(new sink_preview(session));
        preview_sink->initialize(
            displaycapture_source.get(),
            WINDOW_WIDTH, WINDOW_HEIGHT,
            hwnd, d3d11dev, d3d11devctx, swapchain);

        // initialize the topology
        media_stream_t sink_stream = preview_sink->create_stream(topology->get_clock());
        topology->connect_streams(
            displaycapture_source->create_stream(topology->get_clock()), sink_stream);

        // add the topology to the media session
        session->switch_topology(topology);

        // start the media session
        //session->start_playback();
        /*preview_sink->start(*sink_stream);*/
        sink_stream = NULL;

        session->start_playback(0);

        //for(int i = 0; i < 5; i++)
        //{
        //    session->start_playback();
        //    Sleep(10);
        //    session->stop_playback();
        //    //Sleep(1000);
        //}

        MSG msg = {};
        while(GetMessage(&msg, NULL, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        session->stop_playback();

        // shutdown the media session
        session->shutdown();
    }

    /*Sleep(INFINITE);*/


    /*long c = topology.use_count();
    long cc = topology->clock.use_count();
    c = 0;
    topology = NULL;*/

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