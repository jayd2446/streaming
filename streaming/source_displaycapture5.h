#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "media_sample.h"
#include "transform_videoprocessor.h"
#include "control_pipeline.h"
#include "async_callback.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>
#include <queue>

#pragma comment(lib, "dxgi")

class stream_displaycapture5;
class stream_displaycapture5_pointer;
class control_pipeline;
typedef std::shared_ptr<stream_displaycapture5> stream_displaycapture5_t;
typedef std::shared_ptr<stream_displaycapture5_pointer> stream_displaycapture5_pointer_t;

class source_displaycapture5 : public media_source
{
    friend class stream_displaycapture5;
    friend class stream_displaycapture5_pointer;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef std::shared_ptr<std::stack<media_buffer_pooled_texture_t>> samples_pool;
private:
    control_pipeline_t ctrl_pipeline;

    CComPtr<IDXGIOutput5> output;
    CComPtr<IDXGIOutputDuplication> output_duplication;
    DXGI_OUTDUPL_DESC outdupl_desc;
    // first devs are for the output duplicator,
    // second devs for the rendering operations in the pipeline;
    CComPtr<ID3D11Device> d3d11dev, d3d11dev2;
    CComPtr<ID3D11DeviceContext> d3d11devctx, d3d11devctx2;
    CComPtr<ID3D11Texture2D> stage_src, stage_dst;
    UINT output_index;

    std::shared_ptr<std::recursive_mutex> available_samples_mutex, available_pointer_samples_mutex;
    samples_pool available_samples, available_pointer_samples;
    media_buffer_texture_t newest_buffer, newest_pointer_buffer;

    std::recursive_mutex capture_frame_mutex;
    /*media_buffer_lockable_texture_t newest_buffer, newest_pointer_buffer;*/
    /*media_buffer_pointer_shape_t newest_pointer_buffer;*/
    DXGI_OUTDUPL_POINTER_POSITION pointer_position;
    std::vector<BYTE> pointer_buffer;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointer_shape_info;

    context_mutex_t context_mutex;
    bool same_device;

    LARGE_INTEGER last_timestamp;
    time_unit last_timestamp2;

    // queue is used for requests so that the request time matches the
    // fetched displaycapture image
    std::recursive_mutex requests_mutex;
    std::queue<request_packet> requests;

    HRESULT copy_between_adapters(
        ID3D11Device* dst_dev,
        ID3D11Texture2D* dst, 
        ID3D11Device* src_dev,
        ID3D11Texture2D* src);
    HRESULT setup_initial_data(
        const media_buffer_texture_t&);
    HRESULT create_pointer_texture(
        const DXGI_OUTDUPL_FRAME_INFO&, const media_buffer_texture_t&);

    HRESULT reinitialize(UINT output_index);
public:
    source_displaycapture5(const media_session_t& session, context_mutex_t context_mutex);
    ~source_displaycapture5();

    bool capture_frame(
        bool& new_pointer_shape,
        DXGI_OUTDUPL_POINTER_POSITION&,
        media_buffer_texture_t& pointer,
        media_buffer_texture_t&,
        time_unit& timestamp, 
        const presentation_clock_t&);

    stream_displaycapture5_t create_stream();
    stream_displaycapture5_pointer_t create_pointer_stream();

    // currently displaycapture initialization never fails;
    // uses the d3d11 device for capturing that is used in the pipeline
    void initialize(
        const control_pipeline_t&,
        UINT output_index, 
        const CComPtr<ID3D11Device>&,
        const CComPtr<ID3D11DeviceContext>&);
    // creates a d3d11 device that is bound to the adapter index
    void initialize(
        const control_pipeline_t&,
        UINT adapter_index,
        UINT output_index, 
        const CComPtr<IDXGIFactory1>&,
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
    CComPtr<async_callback_t> capture_frame_callback;

    stream_displaycapture5_pointer_t pointer_stream;

    void capture_frame_cb(void*);
public:
    explicit stream_displaycapture5(const source_displaycapture5_t& source);

    void set_pointer_stream(const stream_displaycapture5_pointer_t& s) {this->pointer_stream = s;}

    // called by media session
    result_t request_sample(request_packet&, const media_stream*);
    // called by source_displaycapture
    result_t process_sample(const media_sample&, request_packet&, const media_stream* = NULL);
};

class stream_displaycapture5_pointer : public media_stream
{
    friend class stream_displaycapture5;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    source_displaycapture5_t source;
    media_buffer_texture_t null_buffer;
public:
    explicit stream_displaycapture5_pointer(const source_displaycapture5_t& source);

    /*bool set_pointer_rect(
        media_sample_view_videoprocessor_t& sample_view, 
        const RECT& src_rect_in,
        const RECT& dest_rect_in) const;*/

    void dispatch(
        bool new_pointer_shape, 
        const DXGI_OUTDUPL_POINTER_POSITION&,
        const D3D11_TEXTURE2D_DESC* desktop_desc,
        media_sample_videoprocessor&,
        request_packet&);

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample&, request_packet&, const media_stream* = NULL);
};