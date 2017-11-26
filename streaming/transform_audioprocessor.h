#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "request_packet.h"
#include <mfapi.h>
#include <memory>
#include <mutex>

#pragma comment(lib, "Mfplat.lib")

#define OUT_BUFFER_FRAMES 1024

class transform_audioprocessor : public media_source
{
    friend class stream_audioprocessor;
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    bool running;
    CComPtr<IMFTransform> processor;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    CComPtr<IMFMediaType> input_type, output_type;
    UINT32 channels, sample_rate, block_align;
    std::mutex set_type_mutex;

    void reset_input_type(UINT channels, UINT sample_rate);
public:
    explicit transform_audioprocessor(const media_session_t& session);

    void initialize();
    media_stream_t create_stream();
};

typedef std::shared_ptr<transform_audioprocessor> transform_audioprocessor_t;

class stream_audioprocessor : public media_stream
{
public:
    struct packet {request_packet rp; media_sample_view_t sample_view;};
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    transform_audioprocessor_t transform;
    packet pending_packet;
    void processing_cb(void*);
    bool process_output(IMFSample*);
public:
    explicit stream_audioprocessor(const transform_audioprocessor_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};