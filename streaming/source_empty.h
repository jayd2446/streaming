#pragma once
#include "source_base.h"
#include "media_component.h"
#include "media_stream.h"
#include "transform_videomixer.h"
#include "transform_audiomixer2.h"
#include "transform_aac_encoder.h"
#include <memory>

// TODO: source empty could be a template class

class source_empty_audio final : public source_base<media_component_audiomixer_args>
{
    friend class stream_empty_audio;
public:
    typedef buffer_pool<media_sample_audio_mixer_frames_pooled> buffer_pool_audio_frames_t;
private:
    std::shared_ptr<buffer_pool_audio_frames_t> buffer_pool_audio_frames;
    frame_unit last_frame_end;

    stream_source_base_t create_derived_stream() override;
    bool get_samples_end(time_unit request_time, frame_unit& end) const override;
    void make_request(request_t&, frame_unit frame_end) override;
    void dispatch(request_t&) override;

    // one second
    frame_unit get_maximum_buffer_size() const { return this->session->frame_rate_num; }
public:
    explicit source_empty_audio(const media_session_t& session);
    ~source_empty_audio();

    void initialize(const control_class_t&);
};

typedef std::shared_ptr<source_empty_audio> source_empty_audio_t;

class stream_empty_audio final : 
    public stream_source_base<source_base<media_component_audiomixer_args>>
{
private:
    source_empty_audio_t source;
    void on_component_start(time_unit) override;
public:
    explicit stream_empty_audio(const source_empty_audio_t&);
};

typedef std::shared_ptr<stream_empty_audio> stream_empty_audio_t;

class source_empty_video final : public source_base<media_component_videomixer_args>
{
    friend class stream_empty_video;
public:
    typedef buffer_pool<media_sample_video_mixer_frames_pooled> buffer_pool_video_frames_t;
    static const frame_unit maximum_buffer_size = 60;
private:
    std::shared_ptr<buffer_pool_video_frames_t> buffer_pool_video_frames;
    frame_unit last_frame_end;

    stream_source_base_t create_derived_stream() override;
    bool get_samples_end(time_unit request_time, frame_unit& end) const override;
    void make_request(request_t&, frame_unit frame_end) override;
    void dispatch(request_t&) override;
public:
    explicit source_empty_video(const media_session_t& session);
    ~source_empty_video();

    void initialize(const control_class_t&);
};

typedef std::shared_ptr<source_empty_video> source_empty_video_t;

class stream_empty_video final : 
    public stream_source_base<source_base<media_component_videomixer_args>>
{
private:
    source_empty_video_t source;
    void on_component_start(time_unit) override;
public:
    explicit stream_empty_video(const source_empty_video_t&);
};

typedef std::shared_ptr<stream_empty_video> stream_empty_video_t;