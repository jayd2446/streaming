#pragma once
#include "presentation_clock.h"
#include "media_session.h"
#include "control_scene.h"
#include "transform_aac_encoder.h"
#include "transform_h264_encoder.h"
#include "transform_color_converter.h"
#include "transform_videoprocessor.h"
#include "transform_audioprocessor.h"
#include "transform_audiomix.h"
#include "sink_mpeg2.h"
#include "sink_audio.h"
#include "sink_preview2.h"
#include "source_loopback.h"
#include "source_displaycapture5.h"
#include <atlbase.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>
#include <vector>
#include <memory>

#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "DXGI.lib")

// controls the entire pipeline which includes scene switches;
// hosts the video and audio encoder and sinks aswell

// control classes aren't multithread safe

class control_pipeline
{
    friend class control_scene;
private:
    CHandle stopped_signal;
    HWND preview_wnd;

    presentation_time_source_t time_source;
    media_session_t session;
    // these components are present in every scene
    transform_videoprocessor_t videoprocessor_transform;
    transform_h264_encoder_t h264_encoder_transform;
    transform_color_converter_t color_converter_transform;
    transform_aac_encoder_t aac_encoder_transform;
    sink_mpeg2_t mpeg_sink;
    sink_audio_t audio_sink;
    sink_preview2_t preview_sink;

    control_scene* scene_active;
    std::vector<control_scene> scenes;

    UINT d3d11dev_adapter;
    CComPtr<IDXGIFactory1> dxgifactory;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> devctx;
    std::recursive_mutex context_mutex;

    void reset_components(bool create_new);
    // creates and initializes the component
    // or returns the component from the current scene
    source_audio_t create_audio_source(const std::wstring& id, bool capture);
    source_displaycapture5_t create_displaycapture_source(UINT adapter_ordinal, UINT output_ordinal);
    transform_audiomix_t create_audio_mixer();
public:
    control_pipeline();

    bool is_running() const {return this->scene_active != NULL;}

    void update_preview_size() {this->preview_sink->update_size();}
    void initialize(HWND preview_wnd);

    control_scene& create_scene(const std::wstring& name);
    control_scene& get_scene(int index);
    bool is_active(const control_scene&);
    void set_active(control_scene&);
    // stops the pipeline
    void set_inactive();
};