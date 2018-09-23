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

// queue<tracked_samples> samples_pool(the samples can store both vram and ram backed textures);
// the queue and the mutex are shared ptrs that are referenced by the samples;
// at the destructor of color converter the cyclic dependency between queue and samples are broken
// by clearing the queue

class media_buffer_pooled : public media_buffer_trackable<media_buffer_texture>
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef std::shared_ptr<std::stack<std::shared_ptr<media_buffer_pooled>>> samples_pool;
private:
    std::shared_ptr<std::recursive_mutex> available_samples_mutex;
    samples_pool available_samples;

    void on_delete();
public:
    media_buffer_pooled(const samples_pool&, const std::shared_ptr<std::recursive_mutex>&);
    buffer_t create_pooled_buffer();
};

typedef std::shared_ptr<media_buffer_pooled> media_buffer_pooled_t;

class transform_color_converter : public media_source
{
    friend class stream_color_converter;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef std::shared_ptr<std::stack<media_buffer_pooled_t>> samples_pool;
private:
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11VideoDevice> videodevice;
    CComPtr<ID3D11VideoContext> videocontext;
    CComPtr<ID3D11VideoProcessorEnumerator> enumerator;

    std::shared_ptr<std::recursive_mutex> available_samples_mutex;
    samples_pool available_samples;

    context_mutex_t context_mutex;
public:
    transform_color_converter(const media_session_t& session, context_mutex_t context_mutex);
    ~transform_color_converter();

    HRESULT initialize(const CComPtr<ID3D11Device>&, ID3D11DeviceContext* devctx);
    media_stream_t create_stream();
};

typedef std::shared_ptr<transform_color_converter> transform_color_converter_t;

class stream_color_converter : public media_stream
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<stream_color_converter> async_callback_t;
    struct packet {request_packet rp; media_sample_texture sample_view;};
private:
    transform_color_converter_t transform;
    CComPtr<ID3D11VideoProcessor> videoprocessor;
    packet pending_packet;

    void initialize_buffer(const media_buffer_texture_t&);
    void processing_cb(void*);
public:
    explicit stream_color_converter(const transform_color_converter_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    // called by the downstream from media session
    result_t request_sample(request_packet&, const media_stream*);
    // called by the upstream from media session
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};