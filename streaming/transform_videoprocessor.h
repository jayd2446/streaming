#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include "media_sample.h"
#include <d3d11.h>
#include <atlbase.h>
#include <memory>
#include <mutex>
#include <vector>

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

// TODO: video processor sample view should inherit from a generic texture sample view
// that has the texture buffer and locking capabilities;
// texture sample view in turn inherits from the sample view that has the timestamp field
class media_sample_view_videoprocessor : public media_sample_view_texture
{
public:
    typedef stream_videoprocessor_controller::params_t params_t;
private:
public:
    params_t params;
    media_sample_view_videoprocessor(const params_t&, 
        const media_buffer_texture_t& texture_buffer, view_lock_t = LOCK_BUFFERS);
};

typedef std::shared_ptr<media_sample_view_videoprocessor> media_sample_view_videoprocessor_t;

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
    D3D11_VIDEO_PROCESSOR_CAPS videoprocessor_caps;

    std::recursive_mutex& context_mutex;
public:
    transform_videoprocessor(const media_session_t& session, std::recursive_mutex& context_mutex);

    // TODO: add support for more than max input streams
    void initialize(UINT input_streams, const CComPtr<ID3D11Device>&, ID3D11DeviceContext*);
    stream_videoprocessor_t create_stream();

    UINT max_input_streams() const {return this->videoprocessor_caps.MaxInputStreams;}
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
        media_sample_view_videoprocessor_t sample_view;
    };
private:
    transform_videoprocessor_t transform;
    CComPtr<async_callback_t> processing_callback;
    media_buffer_texture_t output_buffer, output_buffer_null;
    CComPtr<ID3D11VideoProcessorOutputView> output_view;
    RECT output_target_rect;
    std::recursive_mutex mutex;
    bool view_initialized;

    typedef std::pair<packet, const media_stream*> input_streams_t;
    std::vector<input_streams_t> input_streams;
    std::vector<D3D11_VIDEO_PROCESSOR_STREAM> input_streams_params;
    int samples_received;

    void processing_cb(void*);
public:
    explicit stream_videoprocessor(const transform_videoprocessor_t& transform);

    // first input stream will appear topmost(imitates obs source ordering)
    void add_input_stream(const media_stream* stream);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    // called by the downstream from media session
    result_t request_sample(request_packet&, const media_stream*);
    // called by the upstream from media session
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};