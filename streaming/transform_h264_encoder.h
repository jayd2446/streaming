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

// the encoder transform must be recreated to change encoder parameters

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
    // sorted by packet number
    typedef std::map<int, packet> sorted_map_t;
    typedef std::queue<packet> queue_t;
    typedef async_callback<transform_h264_encoder> async_callback_t;
private:
    MFT_INPUT_STREAM_INFO input_stream_info;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    CComPtr<IMFTransform> encoder;
    CComPtr<IMFMediaEventGenerator> event_generator;
    CComPtr<IMFDXGIDeviceManager> devmngr;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<async_callback_t> 
        events_callback, process_input_callback, process_output_callback, processing_callback;
    UINT reset_token;
    std::recursive_mutex samples_mutex, encoder_mutex, processed_samples_mutex;
    std::atomic_int32_t last_packet_number, encoder_requests;

    sorted_map_t samples;
    queue_t processed_samples;

    HRESULT set_input_stream_type();
    HRESULT set_output_stream_type();

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
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    transform_h264_encoder_t transform;
public:
    explicit stream_h264_encoder(const transform_h264_encoder_t& transform);

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    // called by the downstream from media session
    result_t request_sample(request_packet&);
    // called by the upstream from media session
    result_t process_sample(const media_sample_view_t&, request_packet&);
};