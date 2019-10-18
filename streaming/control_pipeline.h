#pragma once
#include "control_class.h"
#include "control_scene.h"
#include "control_preview.h"
#include "media_clock.h"
#include "media_session.h"
#include "media_topology.h"
#include "transform_aac_encoder.h"
#include "transform_h264_encoder.h"
#include "transform_color_converter.h"
#include "transform_videomixer.h"
#include "transform_audiomixer2.h"
#include "sink_video.h"
#include "sink_audio.h"
#include "sink_file.h"
#include "source_buffering.h"
#include "output_file.h"
#include "enable_shared_from_this.h"
#include "wtl.h"
#include <atlbase.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <mutex>
#include <memory>
#include <vector>

// TODO: longer buffering increases processing usage
#define BUFFERING_DEFAULT_VIDEO_LATENCY (SECOND_IN_TIME_UNIT / 2) // 100ms default buffering
#define BUFFERING_DEFAULT_AUDIO_LATENCY (SECOND_IN_TIME_UNIT / 2)

#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "DXGI.lib")
#pragma comment(lib, "D2d1.lib")

typedef std::pair<sink_file_video_t, sink_file_audio_t> sink_mp4_t;

class control_pipeline final : public control_class
{
    friend class control_scene;
private:
    bool recording;
    ATL::CWindow recording_initiator_wnd;
    HWND gui_thread_hwnd;

    media_clock_t time_source;
    media_topology_t video_topology, audio_topology;
    // these components are present in every scene
    transform_videomixer_t videomixer_transform;
    transform_h264_encoder_t h264_encoder_transform;
    transform_color_converter_t color_converter_transform;
    transform_aac_encoder_t aac_encoder_transform;
    transform_audiomixer2_t audiomixer_transform;
    sink_mp4_t mp4_sink;
    sink_video_t mpeg_sink;
    sink_audio_t audio_sink;
    source_buffering_video_t video_buffering_source;
    source_buffering_audio_t audio_buffering_source;

    // active controls must not have duplicate elements
    control_set_t controls;

    // the selected items must be contained in control_pipeline so that
    // the lifetimes are managed
    std::vector<control_class*> selected_controls;

    void activate(const control_set_t& last_set, control_set_t& new_set) override;

    void activate_components();
    void deactivate_components();

    void build_and_switch_topology();
public:
    UINT d3d11dev_adapter;
    CComPtr<IDXGIFactory1> dxgifactory;
    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> devctx;
    CComPtr<ID2D1Device> d2d1dev;
    context_mutex_t context_mutex;
    media_session_t session, audio_session;
    // TODO: set root_scene and preview_control to private;
    // TODO: make sure that these controls are always available
    std::shared_ptr<control_scene> root_scene;
    std::shared_ptr<control_preview> preview_control;
    gui_event_provider event_provider;

    explicit control_pipeline(HWND gui_thread_hwnd);
    ~control_pipeline();

    bool run_in_gui_thread(callable_f);

    enum selection_type { ADD, SET, CLEAR };
    // control class must not be null, unless cleared
    // TODO: unset not possible;
    // multiselection is currently not possible
    void set_selected_control(control_class*, selection_type type = SET);
    const std::vector<control_class*>& get_selected_controls() const { return this->selected_controls; }

    bool is_recording() const {return this->recording;}

    //void set_preview_window(HWND hwnd) {this->preview_hwnd = hwnd;}
    //// TODO: this should return control preview instead of the component
    //const sink_preview2_t& get_preview_window() const {return this->preview_sink;}

    // message is sent to the initiator window when the recording has been stopped
    void start_recording(const std::wstring& filename, ATL::CWindow initiator);
    void stop_recording();

    // releases all circular dependencies
    void shutdown() {this->disable();}
};

// TODO: rename this
typedef std::shared_ptr<control_pipeline> control_pipeline_t;