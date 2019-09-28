#pragma once
#include "async_callback.h"
#include "media_sink.h"
#include "media_stream.h"
#include "transform_videomixer.h"
#include <memory>
#include <queue>
#include <mutex>
#include <atomic>

#pragma comment(lib, "D2d1.lib")
#pragma comment(lib, "Dxgi.lib")

#define MAX_TEXTURE_REQUESTS 1

class control_pipeline;
typedef std::shared_ptr<control_pipeline> control_pipeline_t;

class sink_preview2 : public media_sink
{
    friend class stream_preview2;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    control_pipeline_t ctrl_pipeline;
    mutable std::recursive_mutex mutex;
    volatile int texture_requests;

    std::atomic_bool render;
    media_buffer_texture_t last_buffer;
    HWND hwnd;

    void draw_sample(const media_component_args*);
public:
    explicit sink_preview2(const media_session_t& session);

    void initialize(const control_pipeline_t& ctrl_pipeline, HWND);
    media_stream_t create_stream();

    void clear_preview_wnd();
    media_buffer_texture_t get_last_buffer() const { return std::atomic_load(&this->last_buffer); }

    void set_state(bool render) { this->render = render; }
    void clear_request_count() { scoped_lock lock(this->mutex); this->texture_requests = MAX_TEXTURE_REQUESTS; }
    void request_more_textures() { scoped_lock lock(this->mutex); this->texture_requests++; }
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