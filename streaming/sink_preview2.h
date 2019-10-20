#pragma once
#include "async_callback.h"
#include "media_sink.h"
#include "media_stream.h"
#include "transform_videomixer.h"
#include <memory>
#include <atomic>

class control_pipeline;
typedef std::shared_ptr<control_pipeline> control_pipeline_t;

class sink_preview2 : public media_sink
{
    friend class stream_preview2;
private:
    control_pipeline_t ctrl_pipeline;
    media_buffer_texture_t last_buffer;

    void update_preview_sample(const media_component_args*);
public:
    explicit sink_preview2(const media_session_t& session);

    void initialize(const control_pipeline_t& ctrl_pipeline);
    media_stream_t create_stream();

    media_buffer_texture_t get_last_buffer() const { return std::atomic_load(&this->last_buffer); }
};

typedef std::shared_ptr<sink_preview2> sink_preview2_t;

class stream_preview2 : public media_stream
{
private:
    sink_preview2_t sink;
public:
    explicit stream_preview2(const sink_preview2_t& sink);

    result_t request_sample(const request_packet&, const media_stream*);
    // called by media session
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_preview2> stream_preview2_t;