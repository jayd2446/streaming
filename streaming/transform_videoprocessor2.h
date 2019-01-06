#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include "media_sample.h"
#include "control_class.h"
#include "transform_mixer.h"
#include "transform_h264_encoder.h"
#include "request_packet.h"
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <atlbase.h>
#include <memory>
#include <mutex>
#include <vector>
#include <map>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Dxgi.lib")

class stream_videoprocessor2;
typedef std::shared_ptr<stream_videoprocessor2> stream_videoprocessor2_t;

// controls how the stream should process the input sample
class stream_videoprocessor2_controller
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    // TODO: user param dest rect could have a geometry instead
    struct params_t 
    {
        D2D1_RECT_F source_rect, dest_rect; 
        D2D1::Matrix3x2F source_m, dest_m;
        // only for the user dest param
        bool axis_aligned_clip;
    };
private:
    mutable std::mutex mutex;
    params_t params;
public:
    void get_params(params_t&) const;
    void set_params(const params_t&);
};

typedef std::shared_ptr<stream_videoprocessor2_controller> stream_videoprocessor2_controller_t;

class media_sample_videoprocessor2 : public media_sample_texture
{
public:
    typedef stream_videoprocessor2_controller::params_t params_t;
public:
    params_t params;

    media_sample_videoprocessor2() {}
    explicit media_sample_videoprocessor2(const media_buffer_texture_t& texture_buffer);
    media_sample_videoprocessor2(const params_t&, const media_buffer_texture_t& texture_buffer);
    virtual ~media_sample_videoprocessor2() {}
};

// TODO: videoprocessor controller is no longer needed, the component
// can be directly modified(probably)

class transform_videoprocessor2 : public media_source
{
    friend class stream_videoprocessor2;
public:
    typedef buffer_pool<media_buffer_pooled_texture> buffer_pool;

    // TODO: canvas size should probably be a float
    static const UINT32 canvas_width = transform_h264_encoder::frame_width;
    static const UINT32 canvas_height = transform_h264_encoder::frame_height;
private:
    control_class_t ctrl_pipeline;
    context_mutex_t context_mutex;
    std::shared_ptr<buffer_pool> texture_pool;

    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> d3d11devctx;
    CComPtr<ID2D1Device> d2d1dev;
public:
    transform_videoprocessor2(const media_session_t& session, context_mutex_t context_mutex);
    ~transform_videoprocessor2();

    void initialize(
        const control_class_t&,
        const CComPtr<ID2D1Factory1>&,
        const CComPtr<ID2D1Device>&,
        const CComPtr<ID3D11Device>&,
        const CComPtr<ID3D11DeviceContext>&);
    stream_videoprocessor2_t create_stream();
};

typedef std::shared_ptr<transform_videoprocessor2> transform_videoprocessor2_t;

class stream_videoprocessor2 : public media_stream
{
    friend class transform_videoprocessor2;
private:
    using media_stream::connect_streams;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    struct input_stream_props_t
    {
        media_stream* input_stream;
        stream_videoprocessor2_controller_t user_params;
    };
    struct packet
    {
        media_stream* input_stream;
        media_sample_videoprocessor2 sample;
        bool valid_user_params;
        media_sample_videoprocessor2::params_t user_params;
    };
    typedef std::pair<size_t /*samples received*/, std::shared_ptr<packet[]>> request_t;
    typedef request_queue<request_t> request_queue;
private:
    transform_videoprocessor2_t transform;
    // TODO: enable multithreaded dev context again by having a predefined set
    CComPtr<ID2D1DeviceContext> d2d1devctx;

    request_queue requests;
    std::vector<input_stream_props_t> input_stream_props;

    void initialize_buffer(const media_buffer_texture_t& buffer);
    void process(request_queue::request_t&);
public:
    explicit stream_videoprocessor2(const transform_videoprocessor2_t& transform);

    // first added input stream appears topmost;
    // user_params can be NULL
    void connect_streams(
        const media_stream_t& from,
        const stream_videoprocessor2_controller_t& user_params,
        const media_topology_t&);

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};