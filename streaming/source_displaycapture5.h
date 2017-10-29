#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "media_sample.h"
#include "async_callback.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

#pragma comment(lib, "dxgi")

class source_displaycapture5 : public media_source
{
    friend class stream_displaycapture5;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef std::pair<media_stream*, request_packet> request_t;
private:
    CComPtr<IDXGIOutputDuplication> output_duplication;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> d3d11devctx;

    std::recursive_mutex capture_frame_mutex;
    media_buffer_texture_t newest_buffer;

    std::recursive_mutex requests_mutex;
    std::queue<request_t> requests;

    std::recursive_mutex& context_mutex;
public:
    source_displaycapture5(const media_session_t& session, std::recursive_mutex& context_mutex);

    bool capture_frame(const media_buffer_texture_t&, time_unit& timestamp);

    media_stream_t create_stream();
    // after initializing starts the capturing
    HRESULT initialize(
        UINT output_index, 
        const CComPtr<ID3D11Device>&,
        const CComPtr<ID3D11DeviceContext>&);
};

typedef std::shared_ptr<source_displaycapture5> source_displaycapture5_t;

class stream_displaycapture5 : public media_stream
{
public:
    typedef async_callback<stream_displaycapture5> async_callback_t;
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    source_displaycapture5_t source;
    media_sample_t sample;
    const media_buffer_texture_t buffer;
    CComPtr<async_callback_t> capture_frame_callback;

    void capture_frame_cb(void*);
public:
    explicit stream_displaycapture5(const source_displaycapture5_t& source);

    // called by media session
    result_t request_sample(request_packet&, const media_stream*);
    // called by source_displaycapture
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream* = NULL);
};