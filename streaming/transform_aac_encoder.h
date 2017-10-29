#pragma once

#include "media_source.h"
#include "media_stream.h"
#include <mfapi.h>
#include <memory>
#include <mutex>

#pragma comment(lib, "Mfplat.lib")

class transform_aac_encoder : public media_source
{
    friend class stream_aac_encoder;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef request_queue<WORKER_STREAMS> request_queue_t;
    typedef request_queue_t::request_t request_t;
private:
    CComPtr<IMFTransform> encoder;
    CComPtr<IMFMediaType> input_type;
    MFT_INPUT_STREAM_INFO input_stream_info;
    MFT_OUTPUT_STREAM_INFO output_stream_info;

    std::recursive_mutex encoder_mutex;
    request_queue_t requests;

    DWORD input_id, output_id;

    void processing_cb(void*);
    bool process_output_cb(request_t*, media_buffer_samples_t&);
public:
    CComPtr<IMFMediaType> output_type;

    explicit transform_aac_encoder(const media_session_t& session);

    HRESULT initialize(const CComPtr<IMFMediaType>& input_type);
    media_stream_t create_stream();
};

typedef std::shared_ptr<transform_aac_encoder> transform_aac_encoder_t;

class stream_aac_encoder : public media_stream
{
private:
    transform_aac_encoder_t transform;
public:
    explicit stream_aac_encoder(const transform_aac_encoder_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_aac_encoder> stream_aac_encoder_t;