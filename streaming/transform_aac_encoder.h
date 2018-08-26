#pragma once

#include "media_source.h"
#include "media_stream.h"
#include <mfapi.h>
#include <memory>
#include <mutex>

#pragma comment(lib, "Mfplat.lib")

// aac encoder only accepts 48khz and 2 channel audio

class transform_aac_encoder : public media_source
{
    friend class stream_aac_encoder;
public:
    struct packet
    {
        media_sample_audio audio;
        bool drain;
    };

    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef request_queue<packet> request_queue;
    typedef request_queue::request_t request_t;

    static const UINT32 input_frames = 1024;
    // sample rate must be a predefined value;
    // the possible values are assumed to be known at compile time
    static const UINT32 sample_rate = 44100;
    static const UINT32 channels = 2;
    enum bitrate_t
    {
        rate_96 = (96 * 1000) / 8,
        rate_128 = (128 * 1000) / 8,
        rate_160 = (160 * 1000) / 8,
        rate_196 = (192 * 1000) / 8
    };
    typedef int16_t bit_depth_t;
    static const UINT32 block_align = sizeof(bit_depth_t) * channels;
private:
    CComPtr<IMFTransform> encoder;
    CComPtr<IMFMediaType> input_type;
    MFT_INPUT_STREAM_INFO input_stream_info;
    MFT_OUTPUT_STREAM_INFO output_stream_info;

    std::recursive_mutex encoder_mutex;
    request_queue requests;

    DWORD input_id, output_id;

    // debug
    frame_unit last_time_stamp;

    void processing_cb(void*);
    bool process_output(IMFSample*);
public:
    CComPtr<IMFMediaType> output_type;

    explicit transform_aac_encoder(const media_session_t& session);

    void initialize(bitrate_t bitrate = rate_128);
    media_stream_t create_stream(presentation_clock_t&);
};

typedef std::shared_ptr<transform_aac_encoder> transform_aac_encoder_t;

class stream_aac_encoder : public media_stream_clock_sink
{
private:
    transform_aac_encoder_t transform;

    time_unit drain_point;

    void on_component_start(time_unit);
    void on_component_stop(time_unit);
public:
    explicit stream_aac_encoder(const transform_aac_encoder_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_aac_encoder> stream_aac_encoder_t;