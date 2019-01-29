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
#include <vector>

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
    };
private:
    mutable std::mutex mutex;
    params_t params;
public:
    void get_params(params_t&) const;
    void set_params(const params_t&);
};

typedef std::shared_ptr<stream_videomixer_controller> stream_videomixer_controller_t;

class media_sample_video_mixer_frame : public media_sample_video_frame
{
public:
    // params are considered valid only if the buffer is not silent
    stream_videomixer_controller::params_t params;
};

typedef media_sample_video_frames_template<media_sample_video_mixer_frame>
media_sample_video_mixer_frames;
typedef std::shared_ptr<media_sample_video_mixer_frames> media_sample_video_mixer_frames_t;
typedef buffer_pooled<media_sample_video_mixer_frames> media_sample_video_mixer_frames_pooled;
typedef std::shared_ptr<media_sample_video_mixer_frames_pooled> 
media_sample_video_mixer_frames_pooled_t;

class media_component_videomixer_args : public media_component_frame_args
{
public:
    // if the sample is non-null, it must not be empty;
    // null buffer frames are silent
    media_sample_video_mixer_frames_t sample;
};

typedef std::optional<media_component_videomixer_args> media_component_videomixer_args_t;

typedef transform_mixer<media_component_videomixer_args_t, stream_videomixer_controller, 
    media_component_h264_encoder_args_t> transform_videomixer_base;
typedef stream_mixer<transform_videomixer_base> stream_videomixer_base;
typedef std::shared_ptr<stream_videomixer_base> stream_videomixer_base_t;

class transform_videomixer : public transform_videomixer_base
{
    friend class stream_videomixer;
private:
    struct device_context_resources;
    typedef std::shared_ptr<device_context_resources> device_context_resources_t;
    typedef buffer_pooled<device_context_resources> device_context_resources_pooled;
    typedef std::shared_ptr<device_context_resources_pooled> device_context_resources_pooled_t;
public:
    typedef buffer_pool<media_sample_video_frames_pooled> buffer_pool_video_frames_t;
    typedef buffer_pool<media_sample_video_mixer_frames_pooled> buffer_pool_video_mixer_frames_t;
    typedef buffer_pool<device_context_resources_pooled> buffer_pool;

    // TODO: canvas size should probably be a float
    static const UINT32 canvas_width = transform_h264_encoder::frame_width;
    static const UINT32 canvas_height = transform_h264_encoder::frame_height;
private:
    control_class_t ctrl_pipeline;
    context_mutex_t context_mutex;

    std::shared_ptr<buffer_pool> texture_pool;
    std::shared_ptr<buffer_pool_video_frames_t> buffer_pool_video_frames;
    std::shared_ptr<buffer_pool_video_mixer_frames_t> buffer_pool_video_mixer_frames;

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
    typedef transform_videomixer::device_context_resources_t device_context_resources_t;
    transform_videomixer_t transform;

    void initialize_texture(const media_buffer_texture_t&);
    void initialize_resources(const device_context_resources_t& resources);
    device_context_resources_t acquire_buffer();
    bool move_frames(in_arg_t& to, in_arg_t& from, const in_arg_t& reference,
        frame_unit end, bool discarded);
    void mix(out_arg_t& out_arg, args_t&, frame_unit first, frame_unit end);
public:
    explicit stream_videomixer(const transform_videomixer_t& transform);
};