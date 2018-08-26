#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include <d3d11.h>
#include <atlbase.h>
#include <memory>
#include <mutex>
#include <queue>

// color space converter

class transform_color_converter : public media_source
{
    friend class stream_color_converter;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11VideoDevice> videodevice;
    CComPtr<ID3D11VideoContext> videocontext;
    CComPtr<ID3D11VideoProcessorEnumerator> enumerator;

    context_mutex_t context_mutex;
public:
    transform_color_converter(const media_session_t& session, context_mutex_t context_mutex);

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
    media_buffer_texture_t output_buffer;
    CComPtr<ID3D11VideoProcessorOutputView> output_view;
    CComPtr<ID3D11VideoProcessor> videoprocessor;
    packet pending_packet;

    void processing_cb(void*);
public:
    explicit stream_color_converter(const transform_color_converter_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    // called by the downstream from media session
    result_t request_sample(request_packet&, const media_stream*);
    // called by the upstream from media session
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};