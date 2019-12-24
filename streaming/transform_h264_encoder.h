#pragma once

#include "media_component.h"
#include "media_stream.h"
#include "async_callback.h"
#include "request_dispatcher.h"
#include "request_queue_handler.h"
#include "control_class.h"
#include <d3d11.h>
#include <atlbase.h>
#include <mfapi.h>
#include <codecapi.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>

// h264 encoder
class stream_h264_encoder;
typedef std::shared_ptr<stream_h264_encoder> stream_h264_encoder_t;

// internal to h264 encoder
struct h264_encoder_transform_packet 
{ 
    bool drain, already_served; 
    media_component_h264_encoder_args_t args; 
};

/*

does not use b frames by default

*/

class transform_h264_encoder : 
    public media_component,
    request_queue_handler<h264_encoder_transform_packet>
{
    friend class stream_h264_encoder;
public:
    typedef buffer_pool<media_sample_h264_frames_pooled> buffer_pool_h264_frames_t;
    typedef buffer_pool<media_buffer_memory_pooled> buffer_pool_memory_t;
    typedef h264_encoder_transform_packet packet;
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef async_callback<transform_h264_encoder> async_callback_t;
    typedef request_dispatcher<::request_queue<media_component_h264_video_args_t>::request_t> 
        request_dispatcher;
    typedef request_queue_handler::request_queue request_queue;
    typedef request_queue::request_t request_t;

    /*static const UINT32 frame_width = 1920, frame_height = 1080;*/
    //static const UINT32 avg_bitrate = 10000/*4500*/ * 1000;
    // 0: low quality, 100: high quality
    /*static const UINT32 quality_vs_speed = 50;*/
private:
    control_class_t ctrl_pipeline;
    context_mutex_t context_mutex;

    UINT32 frame_rate_num, frame_rate_den;
    UINT32 frame_width, frame_height;
    UINT32 avg_bitrate, quality_vs_speed;
    eAVEncH264VProfile encoder_profile;

    DWORD input_id, output_id;
    MFT_INPUT_STREAM_INFO input_stream_info;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    CComPtr<IMFTransform> encoder;
    CComPtr<IMFMediaEventGenerator> event_generator;
    CComPtr<IMFDXGIDeviceManager> devmngr;
    CComPtr<async_callback_t> events_callback;
    UINT reset_token;

    std::mutex process_output_mutex;
    std::atomic_int32_t encoder_requests;

    std::shared_ptr<request_dispatcher> dispatcher;
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

    // request_queue_handler
    bool on_serve(request_queue::request_t&);
    request_queue::request_t* next_request();

    void events_cb(void*);
    void process_output_cb(void*);
public:
    CComPtr<IMFMediaType> output_type;

    explicit transform_h264_encoder(const media_session_t& session, context_mutex_t context_mutex);
    ~transform_h264_encoder();

    bool is_encoder_overloading() const {return this->encoder_requests.load() == 0;}

    // passing null d3d device implies that the system memory is used to feed the encoder;
    // software encoder flag overrides d3d device arg;
    // quality_vs_speed: 0: low quality, 100: high quality;
    // avg bitrate is in bits per second;
    // clsid is optional
    void initialize(const control_class_t&,
        const CComPtr<ID3D11Device>&, 
        UINT32 frame_rate_num, UINT32 frame_rate_den,
        UINT32 frame_width, UINT32 frame_height,
        UINT32 avg_bitrate, UINT32 quality_vs_speed,
        eAVEncH264VProfile,
        const CLSID*,
        bool software);
    media_stream_t create_stream(media_message_generator_t&&);
};

typedef std::shared_ptr<transform_h264_encoder> transform_h264_encoder_t;

class stream_h264_encoder : public media_stream_message_listener
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    transform_h264_encoder_t transform;

    std::atomic<bool> stopping;

    void on_component_start(time_unit);
    void on_component_stop(time_unit);
public:
    explicit stream_h264_encoder(const transform_h264_encoder_t& transform);

    transform_h264_encoder_t get_transform() const {return this->transform;}

    // called by the downstream from media session
    result_t request_sample(const request_packet&, const media_stream*);
    // called by the upstream from media session
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};