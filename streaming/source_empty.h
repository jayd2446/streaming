#pragma once
#include "source_base.h"
#include "media_component.h"
#include "media_stream.h"
#include "async_callback.h"
#include "transform_videomixer.h"
#include <memory>

// TODO: source empty could be a template class

class source_empty_audio : public media_component
{
public:
    explicit source_empty_audio(const media_session_t& session);
    media_stream_t create_stream();
};

typedef std::shared_ptr<source_empty_audio> source_empty_audio_t;

class stream_empty_audio : public media_stream
{
public:
    typedef async_callback<stream_empty_audio> async_callback_t;
private:
    source_empty_audio_t source;
    CComPtr<async_callback_t> callback;
    request_packet rp;
    void callback_f(void*);
public:
    explicit stream_empty_audio(const source_empty_audio_t& source);

    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};

class source_empty_video : public source_base<media_component_videomixer_args>
{
    friend class stream_empty_video;
public:
    typedef buffer_pool<media_sample_video_mixer_frames_pooled> buffer_pool_video_frames_t;
    static const frame_unit maximum_buffer_size = 30;
private:
    std::shared_ptr<buffer_pool_video_frames_t> buffer_pool_video_frames;
    frame_unit last_frame_end;

    stream_source_base_t create_derived_stream();
    bool get_samples_end(const request_t&, frame_unit& end);
    void make_request(request_t&, frame_unit frame_end);
    void dispatch(request_t&);
public:
    explicit source_empty_video(const media_session_t& session);
    ~source_empty_video();

    void initialize();
};

typedef std::shared_ptr<source_empty_video> source_empty_video_t;

class stream_empty_video : public stream_source_base<source_base<media_component_videomixer_args>>
{
private:
    source_empty_video_t source;
    void on_component_start(time_unit);
public:
    explicit stream_empty_video(const source_empty_video_t&);
};

typedef std::shared_ptr<stream_empty_video> stream_empty_video_t;