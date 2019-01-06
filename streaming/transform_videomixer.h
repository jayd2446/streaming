#pragma once

#include "transform_mixer.h"
#include "control_class.h"
#include "transform_h264_encoder.h"
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <atlbase.h>
#include <memory>
#include <mutex>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Dxgi.lib")

class stream_videomixer;
typedef std::shared_ptr<stream_videomixer> stream_videomixer_t;

// controls how the stream should process the input sample
class stream_videomixer_controller
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
        // higher values appear on top;
        // sample z order is relative to the user z order;
        // sample z order is treated as unsigned value
        short z_order;
    };
private:
    mutable std::mutex mutex;
    params_t params;
public:
    void get_params(params_t&) const;
    void set_params(const params_t&);
};

typedef std::shared_ptr<stream_videomixer_controller> stream_videomixer_controller_t;

class media_sample_videomixer : public media_sample_video
{
public:
    stream_videomixer_controller::params_t params;

    media_sample_videomixer() {}
    explicit media_sample_videomixer(const media_buffer_texture_t& single_buffer) :
        media_sample_video(single_buffer) {}
    explicit media_sample_videomixer(const stream_videomixer_controller::params_t& params) :
        params(params) {}
    media_sample_videomixer(const stream_videomixer_controller::params_t& params, 
        const media_buffer_texture_t& single_buffer) :
        params(params), media_sample_video(single_buffer) {}
};

typedef transform_mixer<media_sample_videomixer, stream_videomixer_controller, media_sample_video>
transform_videomixer_base;
typedef stream_mixer<transform_videomixer_base> stream_videomixer_base;
typedef std::shared_ptr<stream_videomixer_base> stream_videomixer_base_t;

class transform_videomixer : public transform_videomixer_base
{
    friend class stream_videomixer;
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

    stream_mixer_t create_derived_stream();
public:
    transform_videomixer(const media_session_t& session, context_mutex_t context_mutex);
    ~transform_videomixer();

    void initialize(
        const control_class_t&,
        const CComPtr<ID2D1Factory1>&,
        const CComPtr<ID2D1Device>&,
        const CComPtr<ID3D11Device>&,
        const CComPtr<ID3D11DeviceContext>&);
};

typedef std::shared_ptr<transform_videomixer> transform_videomixer_t;

class stream_videomixer : public stream_videomixer_base
{
private:
    transform_videomixer_t transform;
    // TODO: enable multithreaded dev context again by having a predefined set
    CComPtr<ID2D1DeviceContext> d2d1devctx;

    void initialize_buffer(const media_buffer_texture_t& buffer);
    media_buffer_texture_t acquire_buffer(const std::shared_ptr<transform_videomixer::buffer_pool>&);

    bool move_frames(sample_t& sample, sample_t& old_sample, frame_unit end);
    void mix(out_sample_t& sample, request_t&, frame_unit first, frame_unit end);
public:
    explicit stream_videomixer(const transform_videomixer_t& transform);
};