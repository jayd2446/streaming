#pragma once
#include "source_base.h"
#include "video_source_helper.h"
#include "transform_videomixer.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <memory>

#pragma comment(lib, "dxgi")

// TODO: duplicateoutput will return e_invalidarg if another instance of this
// with the same output is already running;
// this might happen if a scene is briefly switched to an empty scene and back
// and the associated topology of the scene in question hasn't destroyed yet

class stream_displaycapture;
class stream_displaycapture_pointer;

typedef std::shared_ptr<stream_displaycapture> stream_displaycapture_t;
typedef std::shared_ptr<stream_displaycapture_pointer> stream_displaycapture_pointer_t;

// this struct is internal to displaycapture
struct displaycapture_args
{
    media_component_videomixer_args_t args, pointer_args;
};

class source_displaycapture : public source_base<displaycapture_args>
{
    friend class stream_displaycapture;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef buffer_pool<media_buffer_pooled_texture> buffer_pool;
private:
    control_class_t ctrl_pipeline;
    // since the dxgi output duplication seems to use the d3d11 context,
    // the mutex must be locked when capturing a frame
    context_mutex_t context_mutex;

    video_source_helper source_helper, source_pointer_helper;

    DXGI_OUTDUPL_POINTER_POSITION pointer_position;
    std::vector<BYTE> pointer_buffer;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointer_shape_info;
    std::shared_ptr<buffer_pool> available_samples, available_pointer_samples;
    media_buffer_texture_t newest_buffer, newest_pointer_buffer;

    CComPtr<IDXGIOutput5> output;
    CComPtr<IDXGIOutputDuplication> output_duplication;
    DXGI_OUTDUPL_DESC outdupl_desc;
    UINT output_index;
    // first devs are for the output duplicator,
    // second devs for the rendering operations in the pipeline
    CComPtr<ID3D11Device> d3d11dev, d3d11dev2;
    CComPtr<ID3D11DeviceContext> d3d11devctx, d3d11devctx2;
    CComPtr<ID3D11Texture2D> stage_src, stage_dst;
    bool same_device;

    // source_base
    stream_source_base_t create_derived_stream();
    bool get_samples_end(const request_t&, frame_unit& end);
    // TODO: fetch the sample here
    void make_request(request_t&, frame_unit frame_end);
    void dispatch(request_t&);

    media_buffer_texture_t acquire_buffer(const std::shared_ptr<buffer_pool>&);
    HRESULT reinitialize(UINT output_index);
    HRESULT initialize_pointer_texture(media_buffer_texture_t&);
    HRESULT create_pointer_texture(
        const DXGI_OUTDUPL_FRAME_INFO&, media_buffer_texture_t&);
    // updates the pointer_position and newest buffers;
    // the pointer_position is valid only if frame and pointer_frame are non null
    void capture_frame(media_buffer_texture_t& frame, media_buffer_texture_t& pointer_frame);
public:
    source_displaycapture(const media_session_t&, context_mutex_t context_mutex);
    ~source_displaycapture();

    media_stream_t create_pointer_stream(const stream_displaycapture_t&);

    // currently displaycapture initialization never fails;
    // uses the d3d11 device for capturing that is used in the pipeline
    void initialize(
        const control_class_t&,
        UINT output_index,
        const CComPtr<ID3D11Device>&,
        const CComPtr<ID3D11DeviceContext>&);
    // TODO: create a d3d11 device that is bound to the adapter index
};

typedef std::shared_ptr<source_displaycapture> source_displaycapture_t;

class stream_displaycapture : public stream_source_base<source_base<displaycapture_args>>
{
    friend class source_displaycapture;
    friend class stream_displaycapture_pointer;
private:
    source_displaycapture_t source;
    stream_displaycapture_pointer_t pointer_stream;

    void on_component_start(time_unit);
public:
    explicit stream_displaycapture(const source_displaycapture_t&);
};

class stream_displaycapture_pointer : public media_stream
{
    friend class stream_displaycapture;
private:
    source_displaycapture_t source;
public:
    explicit stream_displaycapture_pointer(const source_displaycapture_t&);

    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};