#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include "media_sample.h"
#include "transform_h264_encoder.h"
#include <d3d11.h>
#include <atlbase.h>
#include <memory>
#include <mutex>
#include <vector>

#undef min

// source blender

// TODO: cache the input views

class stream_videoprocessor;
typedef std::shared_ptr<stream_videoprocessor> stream_videoprocessor_t;

// controls how the stream should process the sample;
// controllers must be used instead of using streams themselves
// because there are duplicates of streams to allow non locking multithreading

// TODO: add z ordering
class stream_videoprocessor_controller
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    struct params_t
    {
        bool enable_alpha;
        RECT source_rect, dest_rect;
    };
private:
    mutable std::mutex mutex;
    params_t params;
public:
    void get_params(params_t&) const;
    void set_params(const params_t&);
};

typedef std::shared_ptr<stream_videoprocessor_controller> stream_videoprocessor_controller_t;

class media_sample_videoprocessor : public media_sample_texture
{
public:
    typedef stream_videoprocessor_controller::params_t params_t;
public:
    params_t params;

    media_sample_videoprocessor() {}
    explicit media_sample_videoprocessor(const media_buffer_texture_t& texture_buffer);
    media_sample_videoprocessor(const params_t&, const media_buffer_texture_t& texture_buffer);
    virtual ~media_sample_videoprocessor() {}
};

//typedef media_sample_view<media_sample_videoprocessor> media_sample_view_videoprocessor;

class transform_videoprocessor : public media_source
{
    friend class stream_videoprocessor;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;

    static const UINT32 canvas_width = transform_h264_encoder::frame_width;
    static const UINT32 canvas_height = transform_h264_encoder::frame_height;
private:
    control_pipeline_t ctrl_pipeline;

    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11VideoDevice> videodevice;
    CComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    CComPtr<ID3D11VideoContext> videocontext;
    CComPtr< ID3D11DeviceContext> devctx;
    D3D11_VIDEO_PROCESSOR_CAPS videoprocessor_caps;

    context_mutex_t context_mutex;
public:
    transform_videoprocessor(const media_session_t& session, context_mutex_t context_mutex);

    void initialize(const control_pipeline_t&,
        const CComPtr<ID3D11Device>&, const CComPtr<ID3D11DeviceContext>&);
    stream_videoprocessor_t create_stream();

    UINT max_input_streams() const;
};

typedef std::shared_ptr<transform_videoprocessor> transform_videoprocessor_t;

class stream_videoprocessor : public media_stream
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<stream_videoprocessor> async_callback_t;

    struct packet 
    {
        request_packet rp; 
        media_sample_videoprocessor sample_view;
        stream_videoprocessor_controller_t user_params;
    };
private:
    transform_videoprocessor_t transform;
    CComPtr<ID3D11VideoProcessor> videoprocessor;
    media_buffer_texture_t output_buffer[2];
    CComPtr<ID3D11VideoProcessorOutputView> output_view[2];
    RECT output_target_rect;
    std::recursive_mutex mutex;
    std::vector<D3D11_VIDEO_PROCESSOR_STREAM> streams;

    typedef std::pair<packet, const media_stream*> input_streams_t;
    std::vector<input_streams_t> input_streams;
    int samples_received;

    void release_input_streams(std::vector<D3D11_VIDEO_PROCESSOR_STREAM>& streams);
    HRESULT set_input_stream(
        const media_sample_videoprocessor::params_t& stream_params,
        const media_sample_videoprocessor::params_t& user_params,
        const CComPtr<ID3D11Texture2D>&,
        D3D11_VIDEO_PROCESSOR_STREAM&, 
        UINT index,
        bool&);
    HRESULT blit(
        const std::vector<D3D11_VIDEO_PROCESSOR_STREAM>& streams,
        const CComPtr<ID3D11VideoProcessorOutputView>&);
    // returns false if the stream won't be shown
    bool calculate_stream_rects(
        const media_sample_videoprocessor::params_t& stream_params,
        const media_sample_videoprocessor::params_t& user_params,
        RECT& src_rect, RECT& dst_rect);

    void processing_cb(void*);
public:
    explicit stream_videoprocessor(const transform_videoprocessor_t& transform);

    // last input stream appears topmost;
    // user_params can be NULL
    void add_input_stream(
        const media_stream* stream,
        const stream_videoprocessor_controller_t& user_params);
    // this function is not thread safe
    size_t input_streams_count() const {return this->input_streams.size();}

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    // called by the downstream from media session
    result_t request_sample(request_packet&, const media_stream*);
    // called by the upstream from media session
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};