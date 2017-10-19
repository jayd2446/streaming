#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "async_callback.h"
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

class transform_h264_encoder : public media_source
{
    friend class stream_h264_encoder;
public:
    struct packet 
    {
        request_packet rp; 
        media_sample_view_t sample_view;
        // request packet ensures that the stream will stay alive
        stream_h264_encoder* stream;
    };
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<transform_h264_encoder> async_callback_t;
private:
    DWORD input_id, output_id;
    MFT_INPUT_STREAM_INFO input_stream_info;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    CComPtr<IMFTransform> encoder;
    CComPtr<IMFMediaEventGenerator> event_generator;
    CComPtr<IMFDXGIDeviceManager> devmngr;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<async_callback_t> 
        events_callback, process_input_callback, process_output_callback;
    UINT reset_token;

    std::recursive_mutex encoder_mutex, events_mutex;
    std::atomic_int32_t encoder_requests;

    int last_packet_number;
    std::recursive_mutex requests_mutex;
    std::deque<packet> requests;

    std::recursive_mutex processed_requests_mutex;
    std::deque<packet> processed_requests;

    std::recursive_mutex processed_samples_mutex;
    std::unordered_map<time_unit /*request time*/, packet> processed_samples;

    HRESULT set_input_stream_type();
    HRESULT set_output_stream_type();
    HRESULT set_encoder_parameters();

    void events_cb(void*);
    void processing_cb(void*);
    void process_output_cb(void*);
    void process_input_cb(void*);
public:
    CComPtr<IMFMediaType> output_type;

    explicit transform_h264_encoder(const media_session_t& session);

    HRESULT initialize(const CComPtr<ID3D11Device>&);
    media_stream_t create_stream();
};

typedef std::shared_ptr<transform_h264_encoder> transform_h264_encoder_t;

class stream_h264_encoder : public media_stream
{
    friend class stream_mpeg_host;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    transform_h264_encoder_t transform;
public:
    explicit stream_h264_encoder(const transform_h264_encoder_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    // called by the downstream from media session
    result_t request_sample(request_packet&, const media_stream*);
    // called by the upstream from media session
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};