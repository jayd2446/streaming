#pragma once
#include "presentation_clock.h"
#include "media_session.h"
#include "transform_aac_encoder.h"
#include "transform_h264_encoder.h"
#include "transform_color_converter.h"
#include "transform_videoprocessor.h"
#include "transform_audioprocessor.h"
#include "transform_audiomixer.h"
#include "sink_mpeg2.h"
#include "sink_audio.h"
#include "sink_preview2.h"
#include "sink_file.h"
#include "output_file.h"
#include "enable_shared_from_this.h"
#include <atlbase.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>
#include <vector>
#include <list>
#include <memory>
#include <mutex>
#include <string>

#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "DXGI.lib")

// controls the entire pipeline which includes scene switches;
// hosts the video and audio encoder and sinks aswell

// TODO: pipeline should host components the same way control_scene does

class source_displaycapture5;
typedef std::shared_ptr<source_displaycapture5> source_displaycapture5_t;
class source_wasapi;
typedef std::shared_ptr<source_wasapi> source_wasapi_t;


// each audio source must have an audio processor attached to it for audio buffering
// and resampling to work
typedef std::pair<source_wasapi_t, transform_audioprocessor_t> source_audio_t;
typedef std::pair<sink_file_t, sink_file_t> sink_mp4_t;

class control_scene;

class control_pipeline : public enable_shared_from_this
{
    friend class control_scene;
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    struct mpeg_sink_item
    {
        bool null_file;
        std::wstring filename;
    };
private:
    volatile bool is_shutdown;

    CHandle stopped_signal;
    HWND preview_wnd;

    bool recording_state_change;

    presentation_time_source_t time_source;
    media_session_t session, audio_session;
    // these components are present in every scene
    transform_videoprocessor_t videoprocessor_transform;
    transform_h264_encoder_t h264_encoder_transform;
    transform_color_converter_t color_converter_transform;
    transform_aac_encoder_t aac_encoder_transform;
    transform_audiomixer_t audiomixer_transform;
    sink_preview2_t preview_sink;
    sink_mp4_t mp4_sink;

    mpeg_sink_item item_mpeg_sink;

    std::pair<mpeg_sink_item, sink_mpeg2_t> mpeg_sink;
    std::pair<mpeg_sink_item, sink_audio_t> audio_sink;

    control_scene* scene_active;
    // use list so that the pointer stays valid when adding/erasing scenes
    std::list<control_scene> scenes;

    UINT d3d11dev_adapter;
    CComPtr<IDXGIFactory1> dxgifactory;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> devctx;
    context_mutex_t context_mutex;

    void activate_components();
    void deactivate_components();

    transform_videoprocessor_t create_videoprocessor();
    transform_h264_encoder_t create_h264_encoder(bool null_file);
    transform_color_converter_t create_color_converter(bool null_file);
    sink_preview2_t create_preview_sink(HWND hwnd);
    sink_mp4_t create_mp4_sink(
        bool null_file, const std::wstring& filename,
        const CComPtr<IMFMediaType>& video_input_type,
        const CComPtr<IMFMediaType>& audio_input_type);
    transform_aac_encoder_t create_aac_encoder(bool null_file);
    transform_audiomixer_t create_audio_mixer();
    sink_mpeg2_t create_mpeg_sink(bool null_file);
    sink_audio_t create_audio_sink(bool null_file);

    /*void reset_components(bool create_new);*/
    // creates and initializes the component
    // or returns the component from the current scene
    source_audio_t create_audio_source(const std::wstring& id, bool capture);
    source_displaycapture5_t create_displaycapture_source(UINT adapter_ordinal, UINT output_ordinal);

    // builds the pipeline specific part of the video topology branch
    void build_video_topology_branch(const media_topology_t& video_topology, 
        const media_stream_t& videoprocessor_stream,
        const stream_mpeg2_t& mpeg_sink_stream);
    void build_audio_topology_branch(const media_topology_t& audio_topology,
        const media_stream_t& audiomixer_stream,
        const stream_audio_t& audio_sink_stream);
public:
    // the mutex must be locked before using any of the pipeline/scene functions;
    // all locks should be cleared before locking this, and nothing that may
    // lock should be called while holding this mutex
    std::recursive_mutex mutex;

    control_pipeline();

    bool is_running() const {return this->scene_active != NULL;}
    bool is_recording() const {return this->is_running() && !this->item_mpeg_sink.null_file;}

    void set_preview_state(bool render) {this->preview_sink->set_state(render);}
    void update_preview_size() {this->preview_sink->update_size();}
    void initialize(HWND preview_wnd);

    void set_mpeg_sink_item(const mpeg_sink_item&);

    control_scene& create_scene(const std::wstring& name);
    control_scene& get_scene(int index);
    control_scene* get_active_scene() const;
    bool is_active(const control_scene&);
    void set_active(control_scene&);

    // releases all circular dependencies;
    // control pipeline is invalid after this call
    void shutdown();

    // returns an event handle that is signalled when the recording is ended
    CHandle start_recording(const std::wstring& filename, control_scene&);
    void stop_recording();
};

typedef std::shared_ptr<control_pipeline> control_pipeline_t;