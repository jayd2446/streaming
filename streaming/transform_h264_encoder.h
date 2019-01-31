#pragma once

#include "media_component.h"
#include "media_stream.h"
#include "async_callback.h"
#include "control_class.h"
#include <d3d11.h>
#include <atlbase.h>
#include <mfapi.h>
#include <memory>
#include <mutex>
#include <map>
#include <unordered_map>
#include <queue>
#include <atomic>

// h264 encoder
class stream_h264_encoder;
typedef std::shared_ptr<stream_h264_encoder> stream_h264_encoder_t;

// the encoder transform must be recreated to change encoder parameter
// the encoder currently acts as a sink;
// the first packet must be 0

class transform_h264_encoder : public media_component
{
    friend class stream_h264_encoder;
public:
    typedef buffer_pool<media_sample_h264_frames_pooled> buffer_pool_h264_frames_t;
    typedef buffer_pool<media_buffer_memory_pooled> buffer_pool_memory_t;
    struct packet {bool drain; media_component_h264_encoder_args_t args;};
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<transform_h264_encoder> async_callback_t;
    typedef request_queue<packet> request_queue;
    typedef request_queue::request_t request_t;

    static const UINT32 frame_width = 1920, frame_height = 1080;
    static const UINT32 frame_rate_num = 60;
    static const UINT32 frame_rate_den = 1;
    static const UINT32 avg_bitrate = 10000/*4500*/ * 1000;
    // 0: low quality, 100: high quality
    static const UINT32 quality_vs_speed = 50;
private:
    control_class_t ctrl_pipeline;
    context_mutex_t context_mutex;

    DWORD input_id, output_id;
    MFT_INPUT_STREAM_INFO input_stream_info;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    CComPtr<IMFTransform> encoder;
    CComPtr<IMFMediaEventGenerator> event_generator;
    CComPtr<IMFDXGIDeviceManager> devmngr;
    CComPtr<async_callback_t> events_callback, process_input_callback, process_output_callback;
    UINT reset_token;

    std::recursive_mutex process_mutex, process_output_mutex;
    std::atomic_int32_t encoder_requests;

    // should be accessed only in processing_cb which is singlethreaded
    /*std::queue<request_t> queued_requests;*/
    request_queue requests;
    request_t last_request;
    std::atomic_bool draining;
    bool first_sample;

    std::shared_ptr<buffer_pool_h264_frames_t> buffer_pool_h264_frames;
    std::shared_ptr<buffer_pool_memory_t> buffer_pool_memory;
    media_sample_h264_frames_t out_sample;

    // time shift must be used instead of adjusting the time in the output_file, because
    // it seems that the encoder stores a 'hidden' time field which is
    // used by the media foundation's file sink
    time_unit time_shift;

    bool use_system_memory, software;

    // debug
    time_unit last_time_stamp, last_time_stamp2;
    int last_packet;

    HRESULT set_input_stream_type();
    HRESULT set_output_stream_type();
    HRESULT set_encoder_parameters();

    HRESULT feed_encoder(const media_sample_video_frame&);

    void process_request(const media_sample_h264_frames_t&, request_t&);
    bool process_output(CComPtr<IMFSample>&);

    // returns whether the request can be served
    bool extract_frame(media_sample_video_frame&, const request_t&);

    void events_cb(void*);
    void processing_cb(void*);
    void process_output_cb(void*);
    void process_input_cb(void*);
public:
    CComPtr<IMFMediaType> output_type;

    explicit transform_h264_encoder(const media_session_t& session, context_mutex_t context_mutex);
    ~transform_h264_encoder();

    // passing null d3d device implies that the system memory is used to feed the encoder;
    // software encoder flag overrides d3d device arg
    void initialize(const control_class_t&,
        const CComPtr<ID3D11Device>&, bool software = false);
    media_stream_t create_stream(presentation_clock_t&&);
};

typedef std::shared_ptr<transform_h264_encoder> transform_h264_encoder_t;

class stream_h264_encoder : public media_stream_clock_sink
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    transform_h264_encoder_t transform;

    time_unit drain_point;

    void on_component_start(time_unit);
    void on_component_stop(time_unit);
public:
    explicit stream_h264_encoder(const transform_h264_encoder_t& transform);

    // called by the downstream from media session
    result_t request_sample(const request_packet&, const media_stream*);
    // called by the upstream from media session
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};