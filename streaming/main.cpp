#include <iostream>
#include <Windows.h>
#include <mfapi.h>
#include <d3d11.h>
//#include "source_displaycapture.h"
//#include "source_displaycapture2.h"
//#include "source_displaycapture3.h"
//#include "source_displaycapture4.h"
#include "source_displaycapture5.h"
//#include "sink_preview.h"
#include "media_session.h"
#include "media_topology.h"
#include "transform_videoprocessor.h"
#include "transform_h264_encoder.h"
#include "transform_color_converter.h"
#include "sink_mpeg.h"
#include "sink_preview2.h"
#include "source_audio.h"
#include "source_loopback.h"
#include "transform_aac_encoder.h"
#include <mutex>

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "D3D11.lib")

#ifdef _DEBUG
#define CREATE_DEVICE_DEBUG D3D11_CREATE_DEVICE_DEBUG
#else
#define CREATE_DEVICE_DEBUG 0
#endif

LARGE_INTEGER pc_frequency;

#define WINDOW_WIDTH 1200
#define WINDOW_HEIGHT 800
#define OUTPUT_MONITOR 0
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

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
HWND create_window();

/*
TODO: mfshutdown should be called only after all other threads have terminated
*/

/*

color converter should be used only if needed;
it seems that the amd h264 encoder creates minimal artifacts if the color converter is used

*/

#undef max
//}

void create_streams(
    const media_topology_t& topology,
    const sink_preview2_t& preview_sink2,
    const sink_mpeg_t& mpeg_sink,
    const source_displaycapture5_t& displaycapture_source2,
    const source_displaycapture5_t& displaycapture_source,
    const transform_videoprocessor_t& videoprocessor_transform,
    const transform_color_converter_t& color_converter_transform,
    const transform_h264_encoder_t& h264_encoder_transform)
{
    HRESULT hr = S_OK;

    stream_mpeg_host_t mpeg_stream = mpeg_sink->create_host_stream(topology->get_clock());
    /*stream_mpeg_host_t mpeg_stream_audio = mpeg_sink->create_host_stream(topology->get_clock());*/

    // 87,5 fps with speed;
    // 76,3 fps with balanced;
    // 46,1 fps with quality
    // (full hd)

    // 48000*1000/1024, 1000
    mpeg_stream->set_pull_rate(60, 1);
    /*mpeg_stream_audio->set_pull_rate(48000*1000/1024, 1000*2);*/

    /*for(time_unit i = 0;; i = mpeg_stream->get_next_due_time(i))
    {
        time_unit next_pull = mpeg_stream->get_next_due_time(i);
        std::cout << next_pull << ", " << (i + mpeg_stream->get_pull_interval()) << std::endl;
        system("pause");
    }*/

    /*for(time_unit i = 0; i < std::numeric_limits<time_unit>::max(); i = mpeg_stream->get_next_due_time(i))
    {
        time_unit next_pull = mpeg_stream->get_next_due_time(i);
        std::cout << next_pull << ", " << (i + mpeg_stream->get_pull_interval()) << std::endl;
    }*/

    for(int i = 0; i < WORKER_STREAMS; i++)
    {
        // video
        stream_videoprocessor_t transform_stream = videoprocessor_transform->create_stream();
        stream_mpeg_t worker_stream = mpeg_sink->create_worker_stream();
        media_stream_t encoder_stream = h264_encoder_transform->create_stream();
        media_stream_t color_converter_stream = color_converter_transform->create_stream();
        media_stream_t source_stream = displaycapture_source->create_stream();
        media_stream_t source_stream2 = displaycapture_source2->create_stream();
        media_stream_t preview_stream = preview_sink2->create_stream();

        mpeg_stream->set_encoder_stream(std::dynamic_pointer_cast<stream_h264_encoder>(encoder_stream));
        mpeg_stream->add_worker_stream(worker_stream);
        transform_stream->set_primary_stream(source_stream.get());

        topology->connect_streams(source_stream, transform_stream);
        topology->connect_streams(source_stream2, transform_stream);
        topology->connect_streams(transform_stream, color_converter_stream);
        topology->connect_streams(transform_stream, preview_stream);
        topology->connect_streams(color_converter_stream, encoder_stream);
        topology->connect_streams(encoder_stream, worker_stream);
        topology->connect_streams(worker_stream, mpeg_stream);

        // audio
        /*stream_mpeg_t worker_stream_audio = mpeg_sink->create_worker_stream();
        media_stream_t aac_encoder_stream = aac_encoder_transform->create_stream();
        media_stream_t audio_source_stream = loopback_source->create_stream(topology->get_clock());

        mpeg_stream_audio->set_encoder_stream(std::dynamic_pointer_cast<stream_aac_encoder>(aac_encoder_stream));
        mpeg_stream_audio->add_worker_stream(worker_stream_audio);

        topology->connect_streams(audio_source_stream, aac_encoder_stream);
        topology->connect_streams(aac_encoder_stream, worker_stream_audio);
        topology->connect_streams(worker_stream_audio, mpeg_stream_audio);*/
    }

    // (each thread gets approximately 20ms time slice)

done:
    if(FAILED(hr))
        throw std::exception();
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
    /*DWORD taskgroup_id = 0;
    if(FAILED(MFRegisterPlatformWithMMCSS(L"Capture", &taskgroup_id, AVRT_PRIORITY_NORMAL)))
        throw std::exception();*/

    HWND hwnd = create_window();
    /*ShowWindow(hwnd, SW_FORCEMINIMIZE);*/

    QueryPerformanceFrequency(&pc_frequency);

    // create window, direct3d 11 device, context and swap chain
    /*HWND hwnd = create_window();*/

    CComPtr<IDXGISwapChain> swapchain;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> d3d11devctx;
    std::recursive_mutex context_mutex;

    hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT | CREATE_DEVICE_DEBUG,
        NULL, 0, D3D11_SDK_VERSION, &d3d11dev, NULL, &d3d11devctx);

    /*Sleep(10000);*/

    /*while(true)*/
    {
        UINT outputmonitor_index = OUTPUT_MONITOR;

        // create time source
        presentation_time_source_t time_source(new presentation_time_source);

        // create the session and the topology
        media_session_t session(new media_session);
        media_topology_t topology(new media_topology(time_source));

        // create and initialize the h264 encoder transform
        transform_h264_encoder_t h264_encoder_transform(new transform_h264_encoder(session));
        hr = h264_encoder_transform->initialize(d3d11dev);

        // create and initialize the color converter transform
        transform_color_converter_t color_converter_transform(
            new transform_color_converter(session, context_mutex));
        hr = color_converter_transform->initialize(d3d11dev, d3d11devctx);

        // create and initialize the video processor transform
        transform_videoprocessor_t videoprocessor_transform(
            new transform_videoprocessor(session, context_mutex));
        hr = videoprocessor_transform->initialize(d3d11dev, d3d11devctx);

        // create and initialize the display capture source
        source_displaycapture5_t displaycapture_source(
            new source_displaycapture5(session, context_mutex));
        source_displaycapture5_t displaycapture_source2(
            new source_displaycapture5(session, context_mutex));
        hr = displaycapture_source->initialize(0, d3d11dev, d3d11devctx);
        hr |= displaycapture_source2->initialize(1, d3d11dev, d3d11devctx);
        if(FAILED(hr))
        {
            std::cerr << "could not initialize display capture source" << std::endl;
            system("pause");
            return 0;
        }

        // create and initialize the preview window sink
        /*sink_preview_t preview_sink(new sink_preview(session));*/
        sink_preview2_t preview_sink2(new sink_preview2(session, context_mutex));
        preview_sink2->initialize(
            WINDOW_WIDTH, WINDOW_HEIGHT,
            hwnd, d3d11dev, d3d11devctx, swapchain);

        //// create the audio loopback source
        //source_loopback_t loopback_source(new source_loopback(session));
        //loopback_source->initialize();

        //// create the aac encoder transform
        //transform_aac_encoder_t aac_encoder_transform(new transform_aac_encoder(session));
        //aac_encoder_transform->initialize(loopback_source->waveformat_type);

        // create the mpeg sink
        sink_mpeg_t mpeg_sink(new sink_mpeg(session));
        mpeg_sink->initialize(h264_encoder_transform->output_type);

        // initialize the topology
        create_streams(
            topology,
            preview_sink2,
            mpeg_sink,
            displaycapture_source,
            displaycapture_source2,
            videoprocessor_transform,
            color_converter_transform,
            h264_encoder_transform);

        // start the time source
        time_source->set_current_time(0);
        time_source->start();

        // start the media session with the topology
        session->start_playback(topology, 0);

        bool switched_topology = false;
        MSG msg = {};
        while(GetMessage(&msg, NULL, 0, 0))
        {
            if(msg.message == WM_KEYDOWN)
            {
                /*for(int i = 0; i < 1500; i++)*/
                {
                    outputmonitor_index = (outputmonitor_index + 1) % 2;

                    if(outputmonitor_index)
                    {
                        topology.reset(new media_topology(time_source));
                        create_streams(
                            topology,
                            preview_sink2,
                            mpeg_sink,
                            displaycapture_source2,
                            displaycapture_source,
                            videoprocessor_transform,
                            color_converter_transform,
                            h264_encoder_transform);
                    }
                    else
                    {
                        topology.reset(new media_topology(time_source));
                        create_streams(
                            topology,
                            preview_sink2,
                            mpeg_sink,
                            displaycapture_source,
                            displaycapture_source2,
                            videoprocessor_transform,
                            color_converter_transform,
                            h264_encoder_transform);
                    }

                    /*presentation_clock_t clock;
                    session->get_current_clock(clock);*/

                    // to make sure that the timestamps correlate to packet numbers,
                    // request times must not be used for timestamps;
                    // packet numbers are used for components to stay in track how many
                    // requests need to be served

                    /*
                    in video, the packet number and request time correlation can be assumed,
                    but in audio not;
                    that is because the audio topology is switched immediately
                    */

                    // when using topology switch immediate, h264 encoder optimization cannot
                    // be used;
                    // the request time might be inversed if a new event fires in between
                    // the call to switch topology immediate and get_current_time;
                    // there exists a chance that the new topology is started with a request time
                    // older than what the old topology is currently processing
                    /*session->switch_topology_immediate(topology, clock->get_current_time());*/
                    session->switch_topology(topology);


                    /*

                    the components(mainly sources) should be started @ the initialization;
                    source_loopback and others will choose the first sample
                    based on the request time and discard previous samples;
                    at the clock_start, sources should discard all but one sample;

                    the timestamps need to be aligned if they don't use request time;




                    the timestamps should be adjusted by floor function,
                    this is because early video is not so noticeable as late video
                    compared to audio;
                    the video-audio desync is noticeable after 40ms,
                    while max desync in 30fps video is 33.33 ms
                    <=> floor(timestamp);

                    --------------------------------------------------------------------

                    actually, only audio timestamps need to be aligned so that there's
                    no glitching in the playback

                    --------------------------------------------------------------------

                    video timestamps aligned to sink sample rate vs
                    raw timestamps boils down to the question of whether possible
                    timestamp drift of max sample rate is tolerated or not;
                    timestamps aligned to sink sample rate allow for
                    seamless topology switching;
                    (@ 60fps, video should be aligned)

                    audio timestamps must be aligned to aac encoder sample rate always
                    so that there aren't audio glitches; the drift isn't noticeable
                    at any audio sample rate, though
                    
                    --------------------------------------------------------------------

                    sinks should perform the align for video

                    */

                }
            }
            else
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            /*Sleep(60000);
            break;*/
        }

        session->stop_playback();

        // shutdown the media session
        session->shutdown();
    }


    /*long c = topology.use_count();
    long cc = topology->clock.use_count();
    c = 0;
    topology = NULL;*/

    // main should wait for all threads to terminate before terminating itself;
    // this sleep fakes that functionality;
    // TODO: there now exists a chance that the context mutex has been destroyed while
    // a work queue item is dispatching;
    std::cout << "ending..." << std::endl;
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