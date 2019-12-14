#pragma once
#include "control_class.h"
#include "control_scene.h"
#include "control_preview.h"
#include "gui_threadwnd.h"
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
#include <string_view>

// NOTE: buffering slightly increases processing usage
#define BUFFERING_DEFAULT_VIDEO_LATENCY (SECOND_IN_TIME_UNIT / 2) // 100ms default buffering
#define BUFFERING_DEFAULT_AUDIO_LATENCY (SECOND_IN_TIME_UNIT / 2)

#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "DXGI.lib")
#pragma comment(lib, "D2d1.lib")

typedef std::pair<sink_file_video_t, sink_file_audio_t> sink_mp4_t;

//struct control_session_config
//{
//    frame_unit fps_num, fps_den;
//    // allowed values: 44100 and 48000
//    frame_unit sample_rate;
//
//    /*
//    extended config:
//    LUID adapter; (optional)
//
//    video surface type
//    audio channel info
//
//    TODO: media session could hold a major type, which is either video or audio;
//    the uncompressed subtype then would be the video surface type or the audio channel type
//
//    TODO: session must hold this basic info
//    */
//};


#pragma pack(push, 1)

// TODO: these configs need to be refactored
struct control_video_config
{
    int fps_num, fps_den;

    bool adapter_use_default; // TODO: default values can be displayed as System Default
    LUID adapter;

    // by default, control pipeline treats these as hardware encoders,
    // and falls back to software encoders if activation fails
    bool encoder_use_default;
    CLSID encoder;

    UINT32 width_frame, height_frame;
    UINT32 bitrate; // avg bitrate (in kbps)
    UINT32 quality_vs_speed;
    eAVEncH264VProfile h264_video_profile;

    control_video_config() :
        fps_num(60),
        fps_den(1),
        adapter_use_default(true),
        adapter{0},
        encoder_use_default(true),
        encoder{0},
        width_frame(1920),
        height_frame(1080),
        bitrate(6000),
        quality_vs_speed(100),
        h264_video_profile(eAVEncH264VProfile_Main)
    {
    }
};

struct control_audio_config
{
    // allowed values: 44100 and 48000
    int sample_rate;

    UINT32 channels; // must be 1, 2 or 6
    transform_aac_encoder::bitrate_t bitrate;
    UINT32 profile_level_indication;

    control_audio_config() :
        sample_rate(44100),
        channels(2),
        bitrate(transform_aac_encoder::rate_128),
        profile_level_indication(0x29) // default
    {
    }
};

struct control_pipeline_config
{
    static constexpr int MAGIC_NUMBER = 0xE43AF973;
    // version must be increased every time the config struct is changed;
    // new config versions are only allowed to extend the previous config versions,
    // which also means that only the control_pipeline_config struct can be altered
    // (with padding it is not necessary)
    static constexpr int VERSION = 1;
    static constexpr int LATEST_VERSION = VERSION;

    int magic_number;
    int version;
    control_video_config config_video;
    control_audio_config config_audio;

    control_pipeline_config() :
        magic_number(MAGIC_NUMBER),
        version(LATEST_VERSION)
    {
    }
};
#pragma pack(pop)

class control_pipeline_recording_state_transition_exception : public std::exception {};

class control_pipeline final : public control_class
{
    friend class control_scene;
private:
    bool recording;
    ATL::CWindow recording_initiator_wnd;
    gui_threadwnd wnd_thread;

    media_clock_t time_source;
    media_topology_t video_topology, audio_topology;
    // these components are present in every scene
    transform_h264_encoder_t h264_encoder_transform;
    transform_color_converter_t color_converter_transform;
    transform_aac_encoder_t aac_encoder_transform;
    transform_audiomixer2_t audiomixer_transform;
    sink_mp4_t mp4_sink;
    sink_video_t video_sink;
    sink_audio_t audio_sink;
    source_buffering_video_t video_buffering_source;
    source_buffering_audio_t audio_buffering_source;

    control_pipeline_config config;

    // active controls must not have duplicate elements
    control_set_t controls;

    // the selected items must be contained in control_pipeline so that
    // the lifetimes are managed
    std::vector<control_class*> selected_controls;

    void activate(const control_set_t& last_set, control_set_t& new_set) override;

    void activate_components();
    void deactivate_components();

    void build_and_switch_topology() override;
public:
    // these member variables must not be cached
    UINT d3d11dev_adapter;
    CComPtr<IDXGIFactory1> dxgifactory;
    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> devctx;
    CComPtr<ID2D1Device> d2d1dev;
    context_mutex_t context_mutex;
    media_session_t session, audio_session;
    // TODO: this should be in control_preview
    transform_videomixer_t videomixer_transform;
    // TODO: set root_scene and preview_control to private;
    // TODO: make sure that these controls are always available
    std::shared_ptr<control_scene> root_scene;
    std::shared_ptr<control_preview> preview_control;
    gui_event_provider event_provider;

    control_pipeline();
    ~control_pipeline();

    void run_in_gui_thread(callable_f) override;

    enum selection_type { ADD, SET, CLEAR };
    // control class must not be null, unless cleared
    // TODO: unset not possible;
    // multiselection is currently not possible
    void set_selected_control(control_class*, selection_type type = SET);
    const std::vector<control_class*>& get_selected_controls() const { return this->selected_controls; }

    bool is_recording() const { return this->recording; }

    void get_session_frame_rate(frame_unit& num, frame_unit& den) const;
    frame_unit get_session_sample_rate() const { return this->audio_session->frame_rate_num; }

    const control_pipeline_config& get_current_config() const { return this->config; }
    // stores and applies the new config(by calling activate());
    void apply_config(const control_pipeline_config& new_config);
    // throws on error;
    // loads any config version and fills the rest with default values
    // TODO: add custom error values
    static control_pipeline_config load_config(
        const std::wstring_view& config_file = L"settings.dat");
    // throws on error
    static void save_config(
        const control_pipeline_config& config,
        const std::wstring_view& config_file = L"settings.dat");

    //void set_preview_window(HWND hwnd) {this->preview_hwnd = hwnd;}
    //// TODO: this should return control preview instead of the component
    //const sink_preview2_t& get_preview_window() const {return this->preview_sink;}

    // message is sent to the initiator window when the recording has been stopped;
    // might throw control_pipeline_recording_state_transition_exception
    void start_recording(const std::wstring& filename, ATL::CWindow initiator);
    void stop_recording();

    // releases all circular dependencies
    // TODO: decide if this should be removed
    void shutdown() { this->disable(); }
};

// TODO: rename this
typedef std::shared_ptr<control_pipeline> control_pipeline_t;