#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include <memory>
#include <Windows.h>

class stream_preview;

class sink_preview : public media_sink, public std::enable_shared_from_this<sink_preview>
{
private:
public:
    explicit sink_preview(const media_session_t& session);

    media_stream_t create_stream();

    // (presentation clock can be accessed from media session)
    // set_presentation_clock

    // initializes the window
    void initialize(HWND);

    // begin requesting samples
    bool start(media_stream&);
};

typedef std::shared_ptr<sink_preview> sink_preview_t;

class stream_preview : public media_stream
{
private:
    sink_preview_t sink;
public:
    explicit stream_preview(const sink_preview_t& sink);

    // called by sink_preview
    result_t request_sample();
    // called by media session
    result_t process_sample(const media_sample_t&);
};