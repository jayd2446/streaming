#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
#include <memory>

class source_empty_audio : public media_source
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

class source_empty_video : public media_source
{
public:
    explicit source_empty_video(const media_session_t& session);
    media_stream_t create_stream();
};

typedef std::shared_ptr<source_empty_video> source_empty_video_t;

class stream_empty_video : public media_stream
{
public:
    typedef async_callback<stream_empty_video> async_callback_t;
private:
    source_empty_video_t source;
    media_buffer_texture_t buffer;
    CComPtr<async_callback_t> callback;
    request_packet rp;
    void callback_f(void*);
public:
    explicit stream_empty_video(const source_empty_video_t&);

    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};