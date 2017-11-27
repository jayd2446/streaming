#include "control_pipeline.h"

#ifdef _DEBUG
#define CREATE_DEVICE_DEBUG D3D11_CREATE_DEVICE_DEBUG
#else
#define CREATE_DEVICE_DEBUG 0
#endif

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

control_pipeline::control_pipeline() : 
    preview_wnd(NULL),
    time_source(new presentation_time_source),
    session(new media_session),
    scene_active(NULL),
    d3d11dev_adapter(0),
    stopped_signal(CreateEvent(NULL, TRUE, FALSE, NULL))
{
    if(!this->stopped_signal)
        throw std::exception();

    HRESULT hr = S_OK;
    CComPtr<IDXGIAdapter1> dxgiadapter;

    CHECK_HR(hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&this->dxgifactory));
    CHECK_HR(hr = this->dxgifactory->EnumAdapters1(0, &dxgiadapter));
    CHECK_HR(hr = D3D11CreateDevice(
        dxgiadapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT | CREATE_DEVICE_DEBUG,
        NULL, 0, D3D11_SDK_VERSION, &this->d3d11dev, 
        NULL, &this->devctx));

done:
    if(FAILED(hr))
        throw std::exception();
}

void control_pipeline::reset_components(bool create_new)
{
    if(!create_new)
    {
        this->videoprocessor_transform = NULL;
        this->h264_encoder_transform = NULL;
        this->color_converter_transform = NULL;
        this->preview_sink = NULL;
        this->mpeg_sink = NULL;
        this->aac_encoder_transform = NULL;
        this->audio_sink = NULL;
        return;
    }

    // create and initialize the videoprocessor transform
    this->videoprocessor_transform.reset(new transform_videoprocessor(this->session, this->context_mutex));
    this->videoprocessor_transform->initialize(16, this->d3d11dev, this->devctx);

    // create and initialize the h264 encoder transform
    this->h264_encoder_transform.reset(new transform_h264_encoder(this->session));
    this->h264_encoder_transform->initialize(this->d3d11dev);

    // create and initialize the color converter transform
    this->color_converter_transform.reset(new transform_color_converter(this->session, this->context_mutex));
    this->color_converter_transform->initialize(this->d3d11dev, this->devctx);

    // create and initialize the preview window sink
    this->preview_sink.reset(new sink_preview2(this->session, this->context_mutex));
    this->preview_sink->initialize(this->preview_wnd, this->d3d11dev);

    // create the mpeg2 sink
    this->mpeg_sink.reset(new sink_mpeg2(this->session));

    // create and initialize the aac encoder transform
    this->aac_encoder_transform.reset(new transform_aac_encoder(this->mpeg_sink->audio_session));
    this->aac_encoder_transform->initialize();

    // initialize the mpeg2 sink
    this->mpeg_sink->initialize(
        this->stopped_signal,
        this->h264_encoder_transform->output_type, 
        this->aac_encoder_transform->output_type);

    // create and initialize the audio sink
    this->audio_sink.reset(new sink_audio(this->mpeg_sink->audio_session));
    this->audio_sink->initialize(this->mpeg_sink->get_output());
}

void control_pipeline::initialize(HWND preview_wnd)
{
    this->preview_wnd = preview_wnd;
}

control_scene& control_pipeline::create_scene(const std::wstring& name)
{
    this->scenes.push_back(control_scene(*this));
    this->scenes.back().scene_name = name;
    return this->scenes.back();
}

control_scene& control_pipeline::get_scene(int index)
{
    assert_(index >= 0 && index < (int)this->scenes.size());
    return this->scenes[index];
}

source_loopback_t control_pipeline::create_audio_source(const std::wstring& id, bool capture)
{
    if(this->scene_active)
    {
        for(auto it = this->scene_active->audio_sources.begin(); 
            it != this->scene_active->audio_sources.end();
            it++)
        {
            if(it->first.device_id == id && it->first.capture == capture)
                return it->second;
        }
    }

    source_loopback_t loopback_source(new source_loopback(this->mpeg_sink->audio_session));
    loopback_source->initialize(id, capture);
    return loopback_source;
}

source_displaycapture5_t control_pipeline::create_displaycapture_source(
    UINT adapter_ordinal, UINT output_ordinal)
{
    if(this->scene_active)
    {
        for(auto it = this->scene_active->displaycapture_sources.begin();
            it != this->scene_active->displaycapture_sources.end();
            it++)
        {
            if(it->first.adapter_ordinal == adapter_ordinal && it->first.output_ordinal == output_ordinal)
                return it->second;
        }
    }

    source_displaycapture5_t displaycapture_source(
        new source_displaycapture5(this->session, this->context_mutex));
    if(adapter_ordinal == this->d3d11dev_adapter)
        displaycapture_source->initialize(output_ordinal, this->d3d11dev, this->devctx);
    else
        displaycapture_source->initialize(adapter_ordinal, output_ordinal, 
        this->dxgifactory, this->d3d11dev, this->devctx);
    return displaycapture_source;
}

transform_audiomix_t control_pipeline::create_audio_mixer()
{
    transform_audiomix_t transform_audio_mixer(
        new transform_audiomix(this->mpeg_sink->audio_session));
    transform_audio_mixer->initialize();
    return transform_audio_mixer;
}

transform_audioprocessor_t control_pipeline::create_audio_processor()
{
    // do not share audio processor components between scenes because they are bound
    // to specific sample rates and channels;
    // audio processors can be shared if the input matches in the old and the new one
    transform_audioprocessor_t transform_audio_processor(
        new transform_audioprocessor(this->mpeg_sink->audio_session));
    transform_audio_processor->initialize();
    return transform_audio_processor;
}

void control_pipeline::set_active(control_scene& scene)
{
    const bool starting = !this->scene_active;

    if(starting)
        this->reset_components(true);

    scene.activate_scene();
    if(this->scene_active != NULL)
        this->scene_active->deactivate_scene();
    this->scene_active = &scene;

    if(starting)
    {
        // start the time source
        this->time_source->set_current_time(0);
        this->time_source->start();

        // start the media session with the topology
        this->session->start_playback(this->scene_active->video_topology, 0);
    }
    else
    {
        this->session->switch_topology(this->scene_active->video_topology);
    }
}

#include <iostream>

void control_pipeline::set_inactive()
{
    assert_(this->scene_active);

    this->scene_active->deactivate_scene();
    this->scene_active = NULL;
    this->session->stop_playback();
    this->session->shutdown();
    std::cout << "FORCE STOPPING..." << std::endl;
    this->mpeg_sink->get_output()->force_stop();
    this->reset_components(false);

    WaitForSingleObject(this->stopped_signal, INFINITE);
    // stop the time source last, otherwise the pipeline
    // might stall and never stop
    this->time_source->stop();
}