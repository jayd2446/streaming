#pragma once

#include "media_component.h"
#include "media_stream.h"
#include "async_callback.h"
#include <d3d11.h>
#include <atlbase.h>
#include <memory>
#include <mutex>
#include <stack>

// color space converter

// TODO: allow upscaling and downscaling of the source texture

class transform_color_converter : public media_component
{
    friend class stream_color_converter;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef buffer_pool<media_sample_video_frames_pooled> buffer_pool_video_frames_t;
    typedef buffer_pool<media_buffer_pooled_texture> buffer_pool;
private:
    control_class_t ctrl_pipeline;

    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11VideoDevice> videodevice;
    CComPtr<ID3D11VideoContext> videocontext;
    CComPtr<ID3D11VideoProcessorEnumerator> enumerator;

    std::shared_ptr<buffer_pool> texture_pool;
    std::shared_ptr<buffer_pool_video_frames_t> buffer_pool_video_frames;

    UINT32 frame_width_in, frame_height_in;
    UINT32 frame_width_out, frame_height_out;

    context_mutex_t context_mutex;
public:
    transform_color_converter(const media_session_t& session, context_mutex_t context_mutex);
    ~transform_color_converter();

    void initialize(const control_class_t&,
        UINT32 frame_width_in, UINT32 frame_height_in,
        UINT32 frame_width_out, UINT32 frame_height_out,
        const CComPtr<ID3D11Device>&, ID3D11DeviceContext* devctx);
    media_stream_t create_stream();
};

typedef std::shared_ptr<transform_color_converter> transform_color_converter_t;

class stream_color_converter : public media_stream
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    transform_color_converter_t transform;
    CComPtr<ID3D11VideoProcessor> videoprocessor;

    void initialize_buffer(const media_buffer_texture_t&);
    media_buffer_texture_t acquire_buffer();
    void process(media_component_h264_encoder_args_t& args, const request_packet&);
public:
    explicit stream_color_converter(const transform_color_converter_t& transform);

    // called by the downstream from media session
    result_t request_sample(const request_packet&, const media_stream*);
    // called by the upstream from media session
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};