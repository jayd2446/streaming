#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include "media_sample.h"
#include "control_class.h"
#include "transform_h264_encoder.h"
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <atlbase.h>
#include <memory>
#include <mutex>
#include <vector>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Dxgi.lib")

class stream_videoprocessor2;
typedef std::shared_ptr<stream_videoprocessor2> stream_videoprocessor2_t;

// controls how the stream should process the input sample;
// controllers must be used instead of using streams themselves
// because there are duplicates of streams to allow non locking multithreading
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

class transform_videoprocessor2 : public media_source
{
    friend class stream_videoprocessor2;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;

    // TODO: canvas size should probably be a float
    static const UINT32 canvas_width = transform_h264_encoder::frame_width;
    static const UINT32 canvas_height = transform_h264_encoder::frame_height;
private:
    control_class_t ctrl_pipeline;

    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> d3d11devctx;
    CComPtr<ID2D1Device> d2d1dev;

    context_mutex_t context_mutex;
public:
    transform_videoprocessor2(const media_session_t& session, context_mutex_t context_mutex);

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
private:
    using media_stream::connect_streams;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    
    struct packet
    {
        request_packet rp;
        media_sample_videoprocessor2 sample;
        stream_videoprocessor2_controller_t user_params;
    };
private:
    transform_videoprocessor2_t transform;
    CComPtr<ID2D1DeviceContext> d2d1devctx;

    std::recursive_mutex mutex;

    typedef std::pair<packet, const media_stream*> input_stream_t;
    std::vector<input_stream_t> input_streams;
    int samples_received;

    media_buffer_texture_t output_buffer;
    void process();
public:
    explicit stream_videoprocessor2(const transform_videoprocessor2_t& transform);

    // last added input stream appears topmost;
    // user_params can be NULL
    void connect_streams(
        const media_stream_t& from,
        const stream_videoprocessor2_controller_t& user_params,
        const media_topology_t&);

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};