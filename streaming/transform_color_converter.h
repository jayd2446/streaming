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
// TODO: create a pool of textures that can be reused
// (currently color converter creates a new texture each time)

class transform_color_converter : public media_source
{
    friend class stream_color_converter;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11VideoDevice> videodevice;
    // all interfaces that derive from D3D11DeviceChild are free-threaded(multithreading safe)
    CComPtr<ID3D11VideoProcessor> videoprocessor;
    CComPtr<ID3D11VideoProcessorEnumerator> enumerator;
public:
    explicit transform_color_converter(const media_session_t& session);

    HRESULT initialize(const CComPtr<ID3D11Device>&);
    media_stream_t create_stream(ID3D11DeviceContext*);
};

typedef std::shared_ptr<transform_color_converter> transform_color_converter_t;

class stream_color_converter : public media_stream
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<stream_color_converter> async_callback_t;
    struct packet {request_packet rp; media_sample_view_t sample_view;};
private:
    transform_color_converter_t transform;
    CComPtr<async_callback_t> processing_callback;
    media_sample_t output_sample;
    CComPtr<ID3D11VideoContext> videocontext;
    CComPtr<ID3D11Texture2D> output_texture;
    CComPtr<ID3D11VideoProcessorOutputView> output_view;
    HANDLE output_texture_handle;
    bool view_initialized;
    packet pending_packet;

    void processing_cb(void*);
public:
    stream_color_converter(
        ID3D11DeviceContext*,
        const transform_color_converter_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    // called by the downstream from media session
    result_t request_sample(request_packet&);
    // called by the upstream from media session
    result_t process_sample(const media_sample_view_t&, request_packet&);
};