#pragma once

#include "transform_mixer.h"
#include "control_class.h"
#include "transform_aac_encoder.h"
#include <mfapi.h>
#include <mutex>

#pragma comment(lib, "Mfplat.lib")

class stream_audiomixer2;
typedef std::shared_ptr<stream_audiomixer2> stream_audiomixer2_t;

class stream_audiomixer2_controller
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    struct params_t
    {
        double boost;
    };
private:
    mutable std::mutex mutex;
    params_t params;
public:
    void get_params(params_t&) const;
    void set_params(const params_t&);
};

typedef std::shared_ptr<stream_audiomixer2_controller> stream_audiomixer2_controller_t;

class media_sample_audio_mixer_frame : public media_sample_audio_consecutive_frames
{
public:
    // params are considered valid only if the buffer is not silent
    stream_audiomixer2_controller::params_t params;
};

typedef media_sample_audio_frames_template<media_sample_audio_mixer_frame>
media_sample_audio_mixer_frames;
typedef std::shared_ptr<media_sample_audio_mixer_frames> media_sample_audio_mixer_frames_t;
typedef buffer_pooled<media_sample_audio_mixer_frames> media_sample_audio_mixer_frames_pooled;
typedef std::shared_ptr<media_sample_audio_mixer_frames_pooled>
media_sample_audio_mixer_frames_pooled_t;

class media_component_audiomixer_args : public media_component_frame_args
{
public:
    // if the sample is non-null, it must not be empty;
    // null buffer frames are silent
    media_sample_audio_mixer_frames_t sample;
};

typedef std::optional<media_component_audiomixer_args> media_component_audiomixer_args_t;

typedef transform_mixer<media_component_audiomixer_args_t, stream_audiomixer2_controller,
    media_component_aac_encoder_args_t> transform_audiomixer2_base;
typedef stream_mixer<transform_audiomixer2_base> stream_audiomixer2_base;
typedef std::shared_ptr<stream_audiomixer2_base> stream_audiomixer2_base_t;

class transform_audiomixer2 final : public transform_audiomixer2_base
{
    friend class stream_audiomixer2;
public:
    typedef buffer_pool<media_buffer_memory_pooled> buffer_pool_memory_t;
    typedef buffer_pool<media_sample_audio_frames_pooled> buffer_pool_audio_frames_t;
    typedef buffer_pool<media_sample_audio_mixer_frames_pooled> buffer_pool_audio_mixer_frames_t;
    // the bit depth mixer expects for input samples;
    // resampler should output to this bit depth
    typedef float bit_depth_t;
    static const UINT32 bit_depth = sizeof(bit_depth_t) * 8;
    static const UINT32 block_align = bit_depth / 8 * transform_aac_encoder::channels;
private:
    std::shared_ptr<buffer_pool_memory_t> buffer_pool_memory;
    std::shared_ptr<buffer_pool_audio_frames_t> buffer_pool_audio_frames;
    std::shared_ptr<buffer_pool_audio_mixer_frames_t> buffer_pool_audio_mixer_frames;

    // one second
    frame_unit get_maximum_buffer_size() const { return this->session->frame_rate_num; }
    stream_mixer_t create_derived_stream();
public:
    explicit transform_audiomixer2(const media_session_t& session);
    ~transform_audiomixer2();

    void initialize();
};

typedef std::shared_ptr<transform_audiomixer2> transform_audiomixer2_t;

class stream_audiomixer2 final : public stream_audiomixer2_base
{
private:
    transform_audiomixer2_t transform;

    bool move_frames(in_arg_t& to, in_arg_t& from, const in_arg_t& reference,
        frame_unit end, bool discarded);
    void mix(out_arg_t& out_arg, args_t&, frame_unit first, frame_unit end);
public:
    explicit stream_audiomixer2(const transform_audiomixer2_t& transform);
};