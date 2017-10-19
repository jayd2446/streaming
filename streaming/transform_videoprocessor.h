#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include <d3d11.h>
#include <atlbase.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <queue>

// source blender

class stream_videoprocessor;
typedef std::shared_ptr<stream_videoprocessor> stream_videoprocessor_t;

class transform_videoprocessor : public media_source
{
    friend class stream_videoprocessor;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11VideoDevice> videodevice;
    CComPtr<ID3D11VideoProcessor> videoprocessor;
    CComPtr<ID3D11VideoProcessorEnumerator> enumerator;
public:
    explicit transform_videoprocessor(const media_session_t& session);

    HRESULT initialize(const CComPtr<ID3D11Device>&);
    stream_videoprocessor_t create_stream(ID3D11DeviceContext*);
};

typedef std::shared_ptr<transform_videoprocessor> transform_videoprocessor_t;

class stream_videoprocessor : public media_stream
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<stream_videoprocessor> async_callback_t;
    struct packet {request_packet rp; media_sample_view_t sample_view;};
private:
    transform_videoprocessor_t transform;
    CComPtr<async_callback_t> processing_callback;
    media_sample_texture_t output_sample, null_sample;
    CComPtr<ID3D11VideoContext> videocontext;
    CComPtr<ID3D11VideoProcessorOutputView> output_view;
    std::recursive_mutex mutex;
    bool view_initialized;
    // the primary stream will stay alive as long as this stream
    // because they both are in the same topology
    const media_stream* primary_stream;

    packet pending_request, pending_request2;

    void processing_cb(void*);
public:
    stream_videoprocessor(
        ID3D11DeviceContext*,
        const transform_videoprocessor_t& transform);

    // the secondary stream is blended on to the primary stream;
    // primary stream must be in the same topology as this stream
    void set_primary_stream(const media_stream* stream) {this->primary_stream = stream;}

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    // called by the downstream from media session
    result_t request_sample(request_packet&, const media_stream*);
    // called by the upstream from media session
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};