#pragma once
#include "source_base.h"
#include "video_source_helper.h"
#include "media_component.h"
#include "media_stream.h"
#include "media_sample.h"
#include "transform_videomixer.h"
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

class source_vidcap : public source_base<media_component_videomixer_args_t>
{
    friend class stream_vidcap;
    struct source_reader_callback_t;
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef async_callback<source_vidcap> async_callback_t;
    typedef buffer_pool<media_buffer_pooled_texture> buffer_pool_texture_t;
private:
    control_class_t ctrl_pipeline;

    std::mutex source_helper_mutex;
    video_source_helper source_helper;
    std::shared_ptr<buffer_pool_texture_t> buffer_pool_texture;
    CComPtr<source_reader_callback_t> source_reader_callback;

    UINT32 reset_token;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<IMFDXGIDeviceManager> devmngr;

    UINT32 frame_width, frame_height, fps_num, fps_den;
    CComPtr<IMFMediaType> output_type;
    CComPtr<IMFMediaSource> device;
    /*
    By default, when the application releases the source reader, the source reader 
    shuts down the media source by calling IMFMediaSource::Shutdown on the media source.
    */
    CComPtr<IMFSourceReader> source_reader;
    CComPtr<IMFAttributes> source_reader_attributes;
    std::wstring symbolic_link;

    stream_source_base_t create_derived_stream();
    bool get_samples_end(const request_t&, frame_unit& end);
    void make_request(request_t&, frame_unit frame_end);
    void dispatch(request_t&);

    HRESULT queue_new_capture();
public:
    explicit source_vidcap(const media_session_t& session);
    ~source_vidcap();

    void get_size(UINT32& width, UINT32& height) const
    {width = this->frame_width; height = this->frame_height;}

    void initialize(const control_class_t&, 
        const CComPtr<ID3D11Device>&,
        const std::wstring& symbolic_link);
};

typedef std::shared_ptr<source_vidcap> source_vidcap_t;

class stream_vidcap : public stream_source_base<source_base<media_component_videomixer_args_t>>
{
private:
    source_vidcap_t source;
    void on_component_start(time_unit);
public:
    explicit stream_vidcap(const source_vidcap_t&);
};

typedef std::shared_ptr<stream_vidcap> stream_vidcap_t;