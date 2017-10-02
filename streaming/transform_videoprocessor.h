#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include <d3d11.h>
#include <atlbase.h>
#include <memory>
#include <mutex>

// color space converter

// TODO: the queue system for samples can be generalized;
// the requests and samples are tied to work queues so generalizing them can include
// those

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
    CComPtr<ID3D11VideoContext> videocontext;

    CComPtr<ID3D11Texture2D> output_texture;
    CComPtr<ID3D11VideoProcessorOutputView> output_view;
    bool view_initialized;

    // video context isn't multithreaded so the access must be serialized
    // TODO: this must be generalized to all d3d11 context accesses
    std::recursive_mutex videoprocessor_mutex;
public:
    explicit transform_videoprocessor(const media_session_t& session);

    // TODO: access to device context must be synchronized
    HRESULT initialize(const CComPtr<ID3D11Device>&, ID3D11DeviceContext*);
    media_stream_t create_stream();
};

typedef std::shared_ptr<transform_videoprocessor> transform_videoprocessor_t;

class stream_videoprocessor : public media_stream
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<stream_videoprocessor> async_callback_t;
private:
    transform_videoprocessor_t transform;
    CComPtr<async_callback_t> processing_callback;
    /*AsyncCallback<stream_videoprocessor> processing_callback;*/
    media_sample_t pending_sample, output_sample;
    request_packet current_rp;

    void processing_cb();
public:
    explicit stream_videoprocessor(const transform_videoprocessor_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    // called by the downstream from media session
    result_t request_sample(request_packet&);
    // called by the upstream from media session
    result_t process_sample(const media_sample_t&, request_packet&);
};