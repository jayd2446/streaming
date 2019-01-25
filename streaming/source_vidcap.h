#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "media_sample.h"
#include "async_callback.h"
#include <memory>
#include <string>
#include <queue>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <d3d11.h>

#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mfreadwrite.lib")

// TODO: a preview dialog should be created where the output formats can be selected
// (control class will handle the creation of the dialog and the preview pipeline);
// that preview dialog can be generalized to a properties window

class source_vidcap : public media_source
{
    friend class stream_vidcap;
public:
    typedef async_callback<source_vidcap> async_callback_t;
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef buffer_pool<media_buffer_pooled_texture> buffer_pool_texture_t;
    typedef buffer_pool<media_sample_video_frames_pooled> buffer_pool_video_frames_t;

    struct request_t
    {
        bool drain;
    };
    typedef request_queue<request_t> request_queue;

    // maximum amount of frames source vidcap holds before discarding
    static const frame_unit maximum_buffer_size = 30;
private:
    control_class_t ctrl_pipeline;

    bool started;

    std::mutex captured_video_mutex;
    std::shared_ptr<buffer_pool_texture_t> buffer_pool_texture;
    std::shared_ptr<buffer_pool_video_frames_t> buffer_pool_video_frames;
    media_sample_video_frames_t captured_video;

    CHandle serve_callback_event;
    CComPtr<async_callback_t> serve_callback;
    CComPtr<IMFAsyncResult> serve_callback_result;
    MFWORKITEM_KEY serve_callback_key;
    bool serve_in_wait_queue;

    CComPtr<async_callback_t> capture_callback;

    std::mutex requests_mutex;
    std::queue<request_queue::request_t> requests;

    UINT32 reset_token;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<IMFDXGIDeviceManager> devmngr;

    UINT32 frame_width, frame_height;
    CComPtr<IMFMediaType> output_type;
    CComPtr<IMFMediaSource> device;
    /*
    By default, when the application releases the source reader, the source reader 
    shuts down the media source by calling IMFMediaSource::Shutdown on the media source.
    */
    CComPtr<IMFSourceReader> source_reader;
    CComPtr<IMFAttributes> source_reader_attributes;
    std::wstring symbolic_link;

    HRESULT queue_new_capture();
    void serve_cb(void*);
    void capture_cb(void*);
public:
    explicit source_vidcap(const media_session_t& session);
    ~source_vidcap();

    void get_size(UINT32& width, UINT32& height) const
    {width = this->frame_width; height = this->frame_height;}

    void initialize(const control_class_t&, 
        const CComPtr<ID3D11Device>&,
        const std::wstring& symbolic_link);
    media_stream_t create_stream(presentation_clock_t&&);
};

typedef std::shared_ptr<source_vidcap> source_vidcap_t;

class stream_vidcap : public media_stream_clock_sink
{
private:
    source_vidcap_t source;
    time_unit drain_point;

    void on_stream_stop(time_unit);
public:
    explicit stream_vidcap(const source_vidcap_t&);

    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_vidcap> stream_vidcap_t;