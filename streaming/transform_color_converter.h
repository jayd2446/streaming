#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include <d3d11.h>
#include <atlbase.h>
#include <memory>
#include <mutex>
#include <stack>

// color space converter;
// also provides a pool for samples that are submitted to the encoder

class transform_color_converter : public media_source
{
    friend class stream_color_converter;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef std::shared_ptr<std::stack<media_buffer_pooled_texture_t>> samples_pool;
    typedef buffer_pool<media_buffer_pooled_texture> buffer_pool;
private:
    control_class_t ctrl_pipeline;

    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11VideoDevice> videodevice;
    CComPtr<ID3D11VideoContext> videocontext;
    CComPtr<ID3D11VideoProcessorEnumerator> enumerator;

    std::shared_ptr<buffer_pool> texture_pool;

    context_mutex_t context_mutex;
public:
    transform_color_converter(const media_session_t& session, context_mutex_t context_mutex);
    ~transform_color_converter();

    HRESULT initialize(const control_class_t&,
        const CComPtr<ID3D11Device>&, ID3D11DeviceContext* devctx);
    media_stream_t create_stream();
};

typedef std::shared_ptr<transform_color_converter> transform_color_converter_t;

class stream_color_converter : public media_stream
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<stream_color_converter> async_callback_t;
    struct packet {request_packet rp; media_component_video_args_t args;};
private:
    transform_color_converter_t transform;
    CComPtr<ID3D11VideoProcessor> videoprocessor;
    packet pending_packet;

    void initialize_buffer(const media_buffer_texture_t&);
    void processing_cb(void*);
public:
    explicit stream_color_converter(const transform_color_converter_t& transform);

    // called by the downstream from media session
    result_t request_sample(request_packet&, const media_stream*);
    // called by the upstream from media session
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};