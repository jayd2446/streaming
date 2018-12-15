#pragma once
#include "control_class.h"
#include "control_scene2.h"
#include "presentation_clock.h"
#include "media_session.h"
#include "media_topology.h"
#include "transform_aac_encoder.h"
#include "transform_h264_encoder.h"
#include "transform_color_converter.h"
#include "transform_audiomixer.h"
#include "transform_videoprocessor2.h"
#include "sink_mpeg2.h"
#include "sink_audio.h"
#include "sink_preview2.h"
#include "sink_file.h"
#include "output_file.h"
#include "enable_shared_from_this.h"
#include "control_scene2.h"
#include <atlbase.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <mutex>
#include <memory>
#include <vector>

#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "DXGI.lib")
#pragma comment(lib, "D2d1.lib")

typedef std::pair<sink_file_t, sink_file_t> sink_mp4_t;

class control_pipeline2 : public control_class
{
    friend class control_scene2;
private:
    // restart audio mixer is used so that the audio is cut when recording starts/stops
    bool recording, restart_audiomixer;
    HWND preview_hwnd;
    CHandle stopped_signal;
    std::recursive_mutex pipeline_mutex;

    presentation_time_source_t time_source;
    media_topology_t video_topology, audio_topology;
    // these components are present in every scene
    transform_videoprocessor2_t videoprocessor_transform;
    transform_h264_encoder_t h264_encoder_transform;
    transform_color_converter_t color_converter_transform;
    transform_aac_encoder_t aac_encoder_transform;
    transform_audiomixer_t audiomixer_transform;
    sink_preview2_t preview_sink;
    sink_mp4_t mp4_sink;
    sink_mpeg2_t mpeg_sink;
    sink_audio_t audio_sink;

    // active controls must not have duplicate elements
    control_set_t controls;

    void activate(const control_set_t& last_set, control_set_t& new_set);

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
    control_scene2 root_scene;
    std::vector<control_class_t> selected_items;

    control_pipeline2();

    bool is_recording() const {return this->recording;}

    void set_preview_window(HWND hwnd) {this->preview_hwnd = hwnd;}
    const sink_preview2_t& get_preview_window() const {return this->preview_sink;}

    // returns an event handle that is signalled when the recording is ended
    HANDLE start_recording(const std::wstring& filename);
    void stop_recording();

    // releases all circular dependencies
    void shutdown() {this->disable();}
};

typedef std::shared_ptr<control_pipeline2> control_pipeline2_t;