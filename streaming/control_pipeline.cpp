#include "control_pipeline.h"
#include "control_scene.h"
#include "source_displaycapture5.h"
#include "source_wasapi.h"
#include <iostream>
#include <d3d11_4.h>

#ifdef _DEBUG
#define CREATE_DEVICE_DEBUG D3D11_CREATE_DEVICE_DEBUG
#else
#define CREATE_DEVICE_DEBUG 0
#endif

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

control_pipeline::control_pipeline() : 
    preview_wnd(NULL),
    scene_active(NULL),
    d3d11dev_adapter(0),
    recording_state_change(false),
    context_mutex(new std::recursive_mutex),
    is_shutdown(false)
{
    HRESULT hr = S_OK;
    CComPtr<IDXGIAdapter1> dxgiadapter;
    D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    D3D_FEATURE_LEVEL feature_level;
    CComPtr<ID3D11Multithread> multithread;
    BOOL was_protected;

    CHECK_HR(hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&this->dxgifactory));
    CHECK_HR(hr = this->dxgifactory->EnumAdapters1(this->d3d11dev_adapter, &dxgiadapter));
    CHECK_HR(hr = D3D11CreateDevice(
        dxgiadapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT | CREATE_DEVICE_DEBUG,
        feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION, &this->d3d11dev, 
        &feature_level, &this->devctx));

    // use implicit multithreading protection aswell so that the context cannot be
    // accidentally corrupted
    CHECK_HR(hr = this->devctx->QueryInterface(&multithread));
    was_protected = multithread->SetMultithreadProtected(TRUE);

    this->item_mpeg_sink.null_file = true;

    std::cout << "adapter " << this->d3d11dev_adapter << std::endl;

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void control_pipeline::activate_components()
{
    // activate time source and sessions
    if(!this->time_source)
    {
        this->time_source.reset(new presentation_time_source);
        this->time_source->set_current_time(0);
        // time source must be started early because the audio processor might use the time source
        // before the topology is started
        this->time_source->start();
    }
    if(!this->session)
        this->session.reset(new media_session(this->time_source));
    if(!this->audio_session)
        this->audio_session.reset(new media_session(this->time_source));

    // activate videoprocessor transform
    this->videoprocessor_transform = this->create_videoprocessor();

    // activate h264 encoder transform
    this->h264_encoder_transform = this->create_h264_encoder(this->item_mpeg_sink.null_file);

    // activate color converter transform
    this->color_converter_transform = this->create_color_converter(this->item_mpeg_sink.null_file);

    // activate preview window sink
    this->preview_sink = this->create_preview_sink(this->preview_wnd);

    // activate aac encoder transform
    this->aac_encoder_transform = this->create_aac_encoder(this->item_mpeg_sink.null_file);

    // activate audiomixer transform
    this->audiomixer_transform = this->create_audio_mixer();

    // activate mp4 sink
    this->mp4_sink = this->create_mp4_sink(
        this->item_mpeg_sink.null_file, this->item_mpeg_sink.filename,
        this->h264_encoder_transform->output_type, this->aac_encoder_transform->output_type);

    // activate mpeg sink
    this->mpeg_sink.second = this->create_mpeg_sink(this->item_mpeg_sink.null_file);
    // set the mpeg sink attributes after creating the new sink
    this->mpeg_sink.first = this->item_mpeg_sink;

    // activate audio sink
    this->audio_sink.second = this->create_audio_sink(this->item_mpeg_sink.null_file);
    // set the mpeg sink attributes after creating the new sink
    this->audio_sink.first = this->item_mpeg_sink;
}

void control_pipeline::deactivate_components()
{
    this->videoprocessor_transform = NULL;
    this->h264_encoder_transform = NULL;
    this->color_converter_transform = NULL;
    this->preview_sink = NULL;
    this->mp4_sink = sink_mp4_t(NULL, NULL);
    this->mpeg_sink.second = NULL;
    this->aac_encoder_transform = NULL;
    this->audiomixer_transform = NULL;
    this->audio_sink.second = NULL;
    /*this->session = NULL;
    this->audio_session = NULL;
    this->time_source = NULL;*/
}

transform_videoprocessor_t control_pipeline::create_videoprocessor()
{
    if(this->videoprocessor_transform &&
        this->videoprocessor_transform->get_instance_type() == media_component::INSTANCE_SHAREABLE)
        return this->videoprocessor_transform;

    transform_videoprocessor_t videoprocessor_transform(
        new transform_videoprocessor(this->session, this->context_mutex));
    videoprocessor_transform->initialize(this->shared_from_this<control_pipeline>(),
        this->d3d11dev, this->devctx);
    return videoprocessor_transform;
}

transform_h264_encoder_t control_pipeline::create_h264_encoder(bool null_file)
{
    if(null_file)
        return NULL;
    if(this->h264_encoder_transform)
        return this->h264_encoder_transform;

    // TODO: activating the encoder might fail for random reasons,
    // so notify if the primary encoder cannot be used and use the software encoder as a
    // fallback
    // (signature error during activate call was fixed by a reboot)
    transform_h264_encoder_t h264_encoder_transform;
    try
    {
        h264_encoder_transform.reset(new transform_h264_encoder(this->session, this->context_mutex));
        h264_encoder_transform->initialize(this->d3d11dev, false);
    }
    catch(std::exception)
    {
        std::cout << "using system ram for hardware video encoder" << std::endl;

        try
        {
            // try to initialize the h264 encoder without utilizing vram
            h264_encoder_transform.reset(new transform_h264_encoder(this->session, this->context_mutex));
            h264_encoder_transform->initialize(NULL);
        }
        catch(std::exception)
        {
            std::cout << "using software encoder" << std::endl;
            // use software encoder
            h264_encoder_transform.reset(new transform_h264_encoder(this->session, this->context_mutex));
            h264_encoder_transform->initialize(NULL, true);
        }
    }

    return h264_encoder_transform;
}

transform_color_converter_t control_pipeline::create_color_converter(bool null_file)
{
    if(null_file)
        return NULL;
    if(this->color_converter_transform &&
        this->color_converter_transform->get_instance_type() == media_component::INSTANCE_SHAREABLE)
        return this->color_converter_transform;

    transform_color_converter_t color_converter_transform(
        new transform_color_converter(this->session, this->context_mutex));
    color_converter_transform->initialize(this->shared_from_this<control_pipeline>(),
        this->d3d11dev, this->devctx);
    return color_converter_transform;
}

sink_preview2_t control_pipeline::create_preview_sink(HWND hwnd)
{
    if(this->preview_sink)
        return this->preview_sink;

    sink_preview2_t preview_sink(new sink_preview2(this->session, this->context_mutex));
    preview_sink->initialize(hwnd, this->d3d11dev);
    return preview_sink;
}

sink_mp4_t control_pipeline::create_mp4_sink(
    bool null_file, const std::wstring& filename,
    const CComPtr<IMFMediaType>& video_input_type,
    const CComPtr<IMFMediaType>& audio_input_type)
{
    if(null_file)
        return sink_mp4_t(NULL, NULL);
    if(this->mp4_sink.first && this->mp4_sink.second)
        return this->mp4_sink;

    sink_mp4_t mp4_sink;
    mp4_sink.first.reset(new sink_file(this->session));
    mp4_sink.second.reset(new sink_file(this->audio_session));

    output_file_t file_output(new output_file);
    file_output->initialize(false, this->stopped_signal, video_input_type, audio_input_type);

    mp4_sink.first->initialize(file_output, true);
    mp4_sink.second->initialize(file_output, false);

    return mp4_sink;
}

transform_aac_encoder_t control_pipeline::create_aac_encoder(bool null_file)
{
    if(null_file)
        return NULL;
    if(this->aac_encoder_transform)
        return this->aac_encoder_transform;

    transform_aac_encoder_t aac_encoder_transform(new transform_aac_encoder(this->audio_session));
    aac_encoder_transform->initialize();
    return aac_encoder_transform;
}

transform_audiomixer_t control_pipeline::create_audio_mixer()
{
    if(this->audiomixer_transform && !this->recording_state_change)
        return this->audiomixer_transform;

    transform_audiomixer_t transform_audio_mixer(new transform_audiomixer(this->audio_session));
    transform_audio_mixer->initialize();
    return transform_audio_mixer;
}

sink_mpeg2_t control_pipeline::create_mpeg_sink(bool null_file)
{
    if(this->mpeg_sink.second)
    {
        if(this->mpeg_sink.first.null_file == null_file)
            return this->mpeg_sink.second;
    }
    else
    {
        sink_mpeg2_t mpeg_sink(new sink_mpeg2(this->session, this->audio_session));
        mpeg_sink->initialize();
        return mpeg_sink;
    }

    // mpeg sink is simply reinitialized
    this->mpeg_sink.second->initialize();

    return this->mpeg_sink.second;
}

sink_audio_t control_pipeline::create_audio_sink(bool null_file)
{
    if(this->audio_sink.second)
    {
        if(this->audio_sink.first.null_file == null_file)
            return this->audio_sink.second;
    }

    sink_audio_t audio_sink(new sink_audio(this->audio_session));
    audio_sink->initialize();
    return audio_sink;
}

void control_pipeline::initialize(HWND preview_wnd)
{
    this->preview_wnd = preview_wnd;
}

void control_pipeline::set_mpeg_sink_item(const mpeg_sink_item& item)
{
    this->item_mpeg_sink = item;
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
    auto it = this->scenes.begin();
    for(int i = 0; i < index; i++)
        it++;
    return *it;
}

control_scene* control_pipeline::get_active_scene() const
{
    return this->scene_active;
}

source_audio_t control_pipeline::create_audio_source(const std::wstring& id, bool capture)
{
    if(this->scene_active)
    {
        for(auto it = this->scene_active->audio_sources.begin(); 
            it != this->scene_active->audio_sources.end();
            it++)
        {
            // do not share if the instance is null
            // (happens when the component is initialized the first time
            // or it is a reference)
            if(!it->second.first)
                continue;

            // do not share if the instance is not shareable
            if(it->second.first->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE)
                continue;

            if(it->first.device_id == id && it->first.capture == capture && 
                !this->recording_state_change)
                return it->second;
        }
    }

    source_audio_t audio_source;
    audio_source.first.reset(new source_wasapi(this->audio_session));
    audio_source.second.reset(new transform_audioprocessor(this->audio_session));

    audio_source.first->initialize(this->shared_from_this<control_pipeline>(), id, capture);
    audio_source.second->initialize();
    return audio_source;
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
            if(!it->second)
                continue;

            if(it->second->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE)
                continue;

            if(it->first.adapter_ordinal == adapter_ordinal && 
                it->first.output_ordinal == output_ordinal)
                return it->second;
        }
    }

    source_displaycapture5_t displaycapture_source(
        new source_displaycapture5(this->session, this->context_mutex));
    if(adapter_ordinal == this->d3d11dev_adapter)
        displaycapture_source->initialize(this->shared_from_this<control_pipeline>(), 
            output_ordinal, this->d3d11dev, this->devctx);
    else
        displaycapture_source->initialize(this->shared_from_this<control_pipeline>(), 
            adapter_ordinal, output_ordinal, this->dxgifactory, this->d3d11dev, this->devctx);
    return displaycapture_source;
}

void control_pipeline::build_video_topology_branch(const media_topology_t& video_topology,
    const media_stream_t& videoprocessor_stream,
    const stream_mpeg2_t& mpeg_sink_stream)
{
    stream_mpeg2_worker_t worker_stream = this->mpeg_sink.second->create_worker_stream();
    media_stream_t preview_stream = this->preview_sink->create_stream();

    mpeg_sink_stream->add_worker_stream(worker_stream);

    if(this->item_mpeg_sink.null_file)
    {
        preview_stream->connect_streams(videoprocessor_stream, video_topology);
        worker_stream->connect_streams(preview_stream, video_topology);
    }
    else
    {
        media_stream_t encoder_stream = 
            this->h264_encoder_transform->create_stream(video_topology->get_clock());
        media_stream_t color_converter_stream = this->color_converter_transform->create_stream();
        media_stream_t mp4_stream = this->mp4_sink.first->create_stream(video_topology->get_clock());

        // TODO: encoder stream is redundant
        mpeg_sink_stream->encoder_stream = std::dynamic_pointer_cast<stream_h264_encoder>(encoder_stream);

        preview_stream->connect_streams(videoprocessor_stream, video_topology);
        color_converter_stream->connect_streams(preview_stream, video_topology);
        encoder_stream->connect_streams(color_converter_stream, video_topology);
        mp4_stream->connect_streams(encoder_stream, video_topology);
        worker_stream->connect_streams(mp4_stream, video_topology);
    }

    mpeg_sink_stream->connect_streams(worker_stream, video_topology);
}

void control_pipeline::build_audio_topology_branch(const media_topology_t& audio_topology,
    const media_stream_t& audiomixer_stream,
    const stream_audio_t& audio_sink_stream)
{
    stream_audio_worker_t worker_stream = this->audio_sink.second->create_worker_stream();

    audio_sink_stream->add_worker_stream(worker_stream);

    if(this->item_mpeg_sink.null_file)
    {
        worker_stream->connect_streams(audiomixer_stream, audio_topology);
    }
    else
    {
        media_stream_t encoder_stream = 
            this->aac_encoder_transform->create_stream(audio_topology->get_clock());
        media_stream_t mp4_stream =
            this->mp4_sink.second->create_stream(audio_topology->get_clock());

        encoder_stream->connect_streams(audiomixer_stream, audio_topology);
        mp4_stream->connect_streams(encoder_stream, audio_topology);
        worker_stream->connect_streams(mp4_stream, audio_topology);
    }

    audio_sink_stream->connect_streams(worker_stream, audio_topology);
}

void control_pipeline::set_active(control_scene& scene)
{
    assert_(!this->is_shutdown);

    this->activate_components();

    scene.activate_scene();
    if(this->scene_active && this->scene_active != &scene)
        this->scene_active->deactivate_scene();
    this->scene_active = &scene;

    // mpeg sink ensures atomic topology starting/switching for audio and video
    if(!this->session->started())
    {
        // start the media session with the topology
        // it's ok to start with time point of 0 because the starting happens only once
        // in the lifetime of pipeline;
        this->mpeg_sink.second->start_topologies(
            0, this->scene_active->video_topology, this->scene_active->audio_topology);
    }
    else
    {
        this->mpeg_sink.second->switch_topologies(
            this->scene_active->video_topology, this->scene_active->audio_topology);
    }
}

void control_pipeline::shutdown()
{
    assert_(this->scene_active);

    this->scene_active->deactivate_scene();
    this->scene_active = NULL;
    /*this->session->stop_playback();
    this->session->shutdown();
    this->audio_session->stop_playback();
    this->audio_session->shutdown();*/
    this->deactivate_components();

    this->session->stop_playback();
    this->session->shutdown();
    this->audio_session->stop_playback();
    this->audio_session->shutdown();

    this->session = NULL;
    this->audio_session = NULL;
    this->time_source = NULL;

    this->is_shutdown = true;
    
    // TODO: the pipeline might throw when the media foundation has already been
    // closed while the pipeline is still active;
    // async callback should just ignore the exceptions
}

CHandle control_pipeline::start_recording(const std::wstring& filename, control_scene& scene)
{
    assert_(!this->is_recording());
    HRESULT hr = S_OK;

    mpeg_sink_item item;
    item.null_file = false;
    item.filename = filename;
    this->set_mpeg_sink_item(item);

    assert_(!this->stopped_signal);
    this->stopped_signal.Attach(CreateEvent(NULL, TRUE, FALSE, NULL));
    if(!this->stopped_signal)
        throw HR_EXCEPTION(hr);

    this->recording_state_change = true;
    this->set_active(scene);
    this->recording_state_change = false;

    return CHandle(this->stopped_signal);
}

void control_pipeline::stop_recording()
{
    assert_(!this->item_mpeg_sink.null_file);
    assert_(this->scene_active);

    mpeg_sink_item item;
    item.null_file = true;
    this->set_mpeg_sink_item(item);

    this->recording_state_change = true;
    this->set_active(*this->scene_active);
    this->recording_state_change = false;

    /*WaitForSingleObject(this->stopped_signal, INFINITE);
    ResetEvent(this->stopped_signal);*/
}