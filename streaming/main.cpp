#include <iostream>
#include <Windows.h>
#include <mfapi.h>
#include <d3d11.h>
#include "source_displaycapture.h"
#include "sink_preview.h"
#include "media_session.h"
#include "media_topology.h"

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "D3D11.lib")

LARGE_INTEGER pc_frequency;

int main()
{
    QueryPerformanceFrequency(&pc_frequency);
    HRESULT hr = CoInitializeEx(NULL, COINIT_SPEED_OVER_MEMORY);
    hr = MFStartup(MFSTARTUP_NOSOCKET);
    // create d3d11 device
    CComPtr<ID3D11Device> d3d11dev;
    hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION, &d3d11dev, NULL, NULL);

    media_session_t session(new media_session);
    media_topology_t topology(new media_topology);

    // create and initialize the display capture source
    source_displaycapture_t displaycapture_source(new source_displaycapture(session));
    hr = displaycapture_source->initialize(d3d11dev);
    if(FAILED(hr))
    {
        std::cerr << "could not initialize display capture source" << std::endl;
        system("pause");
        return 0;
    }

    // create and initialize the preview window sink
    sink_preview_t preview_sink(new sink_preview(session));
    preview_sink->initialize(NULL);

    // initialize the topology
    media_stream_t sink_stream = preview_sink->create_stream(topology->get_clock());
    topology->connect_streams(displaycapture_source->create_stream(), sink_stream);

    // add the topology to the media session
    session->switch_topology(topology);

    // start the media session
    session->start_playback();
    /*preview_sink->start(*sink_stream);*/
    sink_stream = NULL;

    while(true)
        Sleep(1000);

    // shutdown the media session
    session->shutdown();

    hr = MFShutdown();

    return 0;
}