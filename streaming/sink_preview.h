#pragma once
#include "async_callback.h"
#include "media_sink.h"
#include "media_stream.h"
#include <Windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>

#include <mfapi.h>
#include <mfreadwrite.h>
#include <mfidl.h>

#include <atlbase.h>
#include <queue>
#include <mutex>
#include <atomic>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Dxgi.lib")
#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mfreadwrite.lib")

class stream_preview;
class source_displaycapture;
class source_displaycapture2;
class source_displaycapture3;

/*

sinks should have a request queue
and sources a response queue

sources are treated as analogous signals by
them handling the sample updates independently

TODO: sample requests should have timestamps attached to them

*/

class sink_preview : public media_sink
{
    friend class stream_preview;
private:
    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<ID2D1HwndRenderTarget> rendertarget;
    CComPtr<ID2D1Device> d2d1dev;
    CComPtr<ID2D1DeviceContext> d2d1devctx;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<IDXGISwapChain1> swapchain;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<IDXGIOutput> dxgioutput;
    CComPtr<ID2D1Bitmap1> d2dtarget_bitmap;
    bool drawn;

    CComPtr<ID3D11DeviceContext> d3d11devctx;
    CComPtr<ID3D11RenderTargetView> render_target_view;
    HWND hwnd;

    UINT reset_token;
    CComPtr<IMFDXGIDeviceManager> devmngr;
    CComPtr<IMFSinkWriter> sink_writer;
    // When you are done using the media sink, call the media sink's IMFMediaSink::Shutdown method.
    // (The sink writer does not shut down the media sink.) 
    // Release the sink writer before calling Shutdown on the media sink.
    CComPtr<IMFByteStream> byte_stream;
    CComPtr<IMFMediaType> mpeg_file_type;
    CComPtr<IMFMediaType> input_media_type;
    DWORD stream_index;

    HRESULT create_output_media_type();
    HRESULT create_input_media_type();
    // TODO: sink writer is able to do some format conversions(color_converter thus not needed)
    HRESULT initialize_sink_writer();
public:
    explicit sink_preview(const media_session_t& session);
    ~sink_preview();

    media_stream_t create_stream(presentation_clock_t&);

    // (presentation clock can be accessed from media session)
    // set_presentation_clock

    // initializes the window
    void initialize(
        UINT32 window_width, UINT32 window_height,
        HWND, 
        CComPtr<ID3D11Device>&, 
        CComPtr<ID3D11DeviceContext>&,
        CComPtr<IDXGISwapChain>&);

    //// begin requesting samples
    //bool start(media_stream&);
};

typedef std::shared_ptr<sink_preview> sink_preview_t;

class stream_preview : public media_stream, public presentation_clock_sink
{
public:
    typedef async_callback<stream_preview> async_callback_t;
    struct request_t {time_unit request_time, timestamp; int packet_number;};
private:
    sink_preview_t sink;
    bool running;
    std::queue<request_t> requests;
    std::atomic_int32_t requests_pending, packet_number;
    
    std::recursive_mutex mutex;
    std::mutex render_mutex;
    CComPtr<async_callback_t> callback;

    bool on_clock_start(time_unit, int packet_number);
    void on_clock_stop(time_unit);
    void scheduled_callback(time_unit due_time);

    void schedule_new(time_unit due_time);
    void push_request(time_unit);
    void request_cb(void*);
public:
    explicit stream_preview(const sink_preview_t& sink);
    ~stream_preview();

    bool get_clock(presentation_clock_t&);

    // called by sink_preview
    result_t request_sample(request_packet&);
    // called by media session
    result_t process_sample(const media_sample_t&, request_packet&);
};

typedef std::shared_ptr<stream_preview> stream_preview_t;