#pragma once

#include "media_source.h"
#include "media_stream.h"
#include <mfapi.h>
#include <memory>
#include <mutex>

#pragma comment(lib, "Mfplat.lib")

class transform_audioprocessor : public media_source
{
    friend class stream_audioprocessor;
private:
    void channel_convert(media_buffer_samples_t& samples);
public:
    explicit transform_audioprocessor(const media_session_t& session);
    media_stream_t create_stream();
};

typedef std::shared_ptr<transform_audioprocessor> transform_audioprocessor_t;

class stream_audioprocessor : public media_stream
{
private:
    transform_audioprocessor_t transform;
public:
    explicit stream_audioprocessor(const transform_audioprocessor_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};