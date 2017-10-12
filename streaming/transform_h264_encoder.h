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
#include <queue>
#include <atomic>

// h264 encoder
class stream_h264_encoder;
typedef std::shared_ptr<stream_h264_encoder> stream_h264_encoder_t;

// TODO: currently some of the messages sent by encoder transform
// might go missing

class transform_h264_encoder : public media_source
{
    friend class stream_h264_encoder;
public:
    struct packet {request_packet rp; media_sample_t sample;};
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    // sorted by packet number
    typedef std::map<int, packet> sorted_map_t;
    typedef std::queue<packet> queue_t;
    typedef async_callback<transform_h264_encoder> async_callback_t;
    typedef async_callback<stream_h264_encoder> stream_async_callback_t;
private:
    bool stream_started;
    MFT_INPUT_STREAM_INFO input_stream_info;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    CComPtr<IMFTransform> encoder;
    CComPtr<IMFMediaEventGenerator> event_generator;
    CComPtr<IMFDXGIDeviceManager> devmngr;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<async_callback_t> events_callback;
    CComPtr<stream_async_callback_t> stream_events_callback;
    UINT reset_token;
    std::recursive_mutex requests_mutex, encoder_mutex, create_stream_mutex, processed_requests_mutex;
    std::atomic_int32_t last_packet_number;
    
    sorted_map_t requests;
    queue_t processed_requests;

    HRESULT set_input_stream_type();
    HRESULT set_output_stream_type();
    void events_cb(void*);
public:
    explicit transform_h264_encoder(const media_session_t& session);

    HRESULT initialize(const CComPtr<ID3D11Device>&);
    media_stream_t create_stream();
};

typedef std::shared_ptr<transform_h264_encoder> transform_h264_encoder_t;

class stream_h264_encoder : public media_stream
{
    friend class transform_h264_encoder;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef async_callback<stream_h264_encoder> async_callback_t;
private:
    transform_h264_encoder_t transform;
    CComPtr<async_callback_t> processing_callback;
    CComPtr<async_callback_t> process_output_callback, process_input_callback;
    CComPtr<async_callback_t> events_callback;
    std::atomic_int32_t encoder_requests;

    void processing_cb(void*);
    void events_cb(void*);
    void process_output_cb(void*);
    void process_input_cb(void*);
public:
    explicit stream_h264_encoder(const transform_h264_encoder_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    // called by the downstream from media session
    result_t request_sample(request_packet&);
    // called by the upstream from media session
    result_t process_sample(const media_sample_view&, request_packet&);
};