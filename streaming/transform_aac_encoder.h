#pragma once

#include "media_component.h"
#include "media_stream.h"
#include "request_dispatcher.h"
#include "request_queue_handler.h"
#include <mfapi.h>
#include <memory>
#include <mutex>
#include <atomic>

#pragma comment(lib, "Mfplat.lib")

// internal to aac encoder
struct aac_encoder_transform_packet
{
    media_component_aac_encoder_args_t args;
    media_sample_aac_frames_t out_sample;
    bool drain;
};

class transform_aac_encoder : 
    public media_component,
    request_queue_handler<aac_encoder_transform_packet>
{
    friend class stream_aac_encoder;
public:
    typedef buffer_pool<media_buffer_memory_pooled> buffer_pool_memory_t;
    typedef aac_encoder_transform_packet packet;
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef request_dispatcher<::request_queue<media_component_aac_audio_args_t>::request_t>
        request_dispatcher;
    typedef request_queue_handler::request_queue request_queue;
    typedef request_queue::request_t request_t;

    static const UINT32 channels = 2;
    enum bitrate_t
    {
        rate_96 = (96 * 1000) / 8,
        rate_128 = (128 * 1000) / 8,
        rate_160 = (160 * 1000) / 8,
        rate_196 = (192 * 1000) / 8
    };
    typedef int16_t bit_depth_t;
    static const UINT32 bit_depth = sizeof(bit_depth_t) * 8;
    /*static const UINT32 block_align = sizeof(bit_depth_t) * channels;*/
private:
    CComPtr<IMFTransform> encoder;
    CComPtr<IMFMediaType> input_type;
    MFT_INPUT_STREAM_INFO input_stream_info;
    MFT_OUTPUT_STREAM_INFO output_stream_info;

    std::shared_ptr<request_dispatcher> dispatcher;
    std::vector<media_buffer_memory_t> memory_hosts;
    std::shared_ptr<buffer_pool_memory_t> buffer_pool_memory;
    media_sample_aac_frames_t encoded_audio;

    DWORD input_id, output_id;

    // time shift must be used instead of adjusting the time in the output_file, because
    // it seems that the encoder stores a 'hidden' time field which is
    // used by the media foundation's file sink
    time_unit time_shift;

    // debug
    frame_unit last_time_stamp;

    // request_queue_handler
    bool on_serve(request_queue::request_t&);
    request_queue::request_t* next_request();

    bool encode(const media_sample_audio_frames*, media_sample_aac_frames&, bool drain);
    bool process_output(IMFSample*);
public:
    CComPtr<IMFMediaType> output_type;

    explicit transform_aac_encoder(const media_session_t& session);
    ~transform_aac_encoder();

    void initialize(bitrate_t bitrate = rate_128);
    media_stream_t create_stream(media_message_generator_t&&);
};

typedef std::shared_ptr<transform_aac_encoder> transform_aac_encoder_t;

class stream_aac_encoder : public media_stream_message_listener
{
public:
    typedef buffer_pool<media_sample_aac_frames_pooled> buffer_pool_aac_frames_t;
private:
    transform_aac_encoder_t transform;

    std::shared_ptr<buffer_pool_aac_frames_t> buffer_pool_aac_frames;

    std::atomic<bool> stopping;

    void on_component_start(time_unit);
    void on_component_stop(time_unit);
public:
    explicit stream_aac_encoder(const transform_aac_encoder_t& transform);
    ~stream_aac_encoder();

    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_aac_encoder> stream_aac_encoder_t;