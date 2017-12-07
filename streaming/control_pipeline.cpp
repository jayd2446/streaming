#include "control_pipeline.h"

#ifdef _DEBUG
#define CREATE_DEVICE_DEBUG D3D11_CREATE_DEVICE_DEBUG
#else
#define CREATE_DEVICE_DEBUG 0
#endif

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

control_pipeline::control_pipeline() : 
    preview_wnd(NULL),
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

    this->item_mpeg_sink.null_file = true;

done:
    if(FAILED(hr))
        throw std::exception();
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

    // activate mpeg sink
    this->mpeg_sink.second = this->create_mpeg_sink(
        this->item_mpeg_sink.null_file, this->item_mpeg_sink.filename,
        this->h264_encoder_transform->output_type, this->aac_encoder_transform->output_type);
    // set the mpeg sink attributes after creating the new sink
    this->mpeg_sink.first = this->item_mpeg_sink;

    // activate audio sink
    this->audio_sink.second = this->create_audio_sink(
        this->item_mpeg_sink.null_file, this->mpeg_sink.second->get_output());
    // set the mpeg sink attributes after creating the new sink
    this->audio_sink.first = this->item_mpeg_sink;
}

void control_pipeline::deactivate_components()
{
    this->videoprocessor_transform = NULL;
    this->h264_encoder_transform = NULL;
    this->color_converter_transform = NULL;
    this->preview_sink = NULL;
    this->mpeg_sink.second = NULL;
    this->aac_encoder_transform = NULL;
    this->audio_sink.second = NULL;
    this->session = NULL;
    this->audio_session = NULL;
    this->time_source = NULL;
}

transform_videoprocessor_t control_pipeline::create_videoprocessor()
{
    if(this->videoprocessor_transform)
        return this->videoprocessor_transform;

    // TODO: nvidia video processor accepts less than 16 streams
    transform_videoprocessor_t videoprocessor_transform(
        new transform_videoprocessor(this->session, this->context_mutex));
    videoprocessor_transform->initialize(16, this->d3d11dev, this->devctx);
    return videoprocessor_transform;
}

transform_h264_encoder_t control_pipeline::create_h264_encoder(bool null_file)
{
    if(null_file)
        return NULL;
    if(this->h264_encoder_transform)
        return this->h264_encoder_transform;

    transform_h264_encoder_t h264_encoder_transform(
        new transform_h264_encoder(this->session, this->context_mutex));
    h264_encoder_transform->initialize(this->d3d11dev);
    return h264_encoder_transform;
}

transform_color_converter_t control_pipeline::create_color_converter(bool null_file)
{
    if(null_file)
        return NULL;
    if(this->color_converter_transform)
        return this->color_converter_transform;

    transform_color_converter_t color_converter_transform(
        new transform_color_converter(this->session, this->context_mutex));
    color_converter_transform->initialize(this->d3d11dev, this->devctx);
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

sink_mpeg2_t control_pipeline::create_mpeg_sink(
    bool null_file, const std::wstring& filename,
    const CComPtr<IMFMediaType>& video_input_type,
    const CComPtr<IMFMediaType>& audio_input_type)
{
    if(this->mpeg_sink.second)
    {
        if(this->mpeg_sink.first.null_file == null_file)
            return this->mpeg_sink.second;
    }

    sink_mpeg2_t mpeg_sink(new sink_mpeg2(this->session, this->audio_session));
    mpeg_sink->initialize(
        null_file, this->stopped_signal,
        video_input_type, audio_input_type);
    return mpeg_sink;
}

sink_audio_t control_pipeline::create_audio_sink(bool null_file, const output_file_t& output)
{
    if(this->audio_sink.second)
    {
        if(this->audio_sink.first.null_file == null_file)
            return this->audio_sink.second;
    }

    sink_audio_t audio_sink(new sink_audio(this->audio_session));
    audio_sink->initialize(output);
    return audio_sink;
}

//void control_pipeline::reset_components(bool create_new)
//{
//    if(!create_new)
//    {
//        this->videoprocessor_transform = NULL;
//        this->h264_encoder_transform = NULL;
//        this->color_converter_transform = NULL;
//        this->preview_sink = NULL;
//        this->mpeg_sink = NULL;
//        this->aac_encoder_transform = NULL;
//        this->audio_sink = NULL;
//        this->session = NULL;
//        this->audio_session = NULL;
//        this->time_source = NULL;
//        return;
//    }
//
//    // create the time source and session
//    this->time_source.reset(new presentation_time_source);
//    this->time_source->set_current_time(0);
//    // time source must be started early because the audio processor might use the time source
//    // before the topology is started
//    this->time_source->start();
//    this->session.reset(new media_session(this->time_source));
//
//    // create and initialize the videoprocessor transform
//    this->videoprocessor_transform.reset(new transform_videoprocessor(this->session, this->context_mutex));
//    this->videoprocessor_transform->initialize(16, this->d3d11dev, this->devctx);
//
//    // create and initialize the h264 encoder transform
//    this->h264_encoder_transform.reset(new transform_h264_encoder(this->session));
//    this->h264_encoder_transform->initialize(this->d3d11dev);
//
//    // create and initialize the color converter transform
//    this->color_converter_transform.reset(new transform_color_converter(this->session, this->context_mutex));
//    this->color_converter_transform->initialize(this->d3d11dev, this->devctx);
//
//    // create and initialize the preview window sink
//    this->preview_sink.reset(new sink_preview2(this->session, this->context_mutex));
//    this->preview_sink->initialize(this->preview_wnd, this->d3d11dev);
//
//    // create the mpeg2 sink
//    this->mpeg_sink.reset(new sink_mpeg2(this->session));
//
//    // create and initialize the aac encoder transform
//    this->aac_encoder_transform.reset(new transform_aac_encoder(this->mpeg_sink->audio_session));
//    this->aac_encoder_transform->initialize();
//
//    // initialize the mpeg2 sink
//    this->mpeg_sink->initialize(
//        this->stopped_signal,
//        this->h264_encoder_transform->output_type, 
//        this->aac_encoder_transform->output_type);
//
//    // create and initialize the audio sink
//    this->audio_sink.reset(new sink_audio(this->mpeg_sink->audio_session));
//    this->audio_sink->initialize(this->mpeg_sink->get_output());
//}

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

source_audio_t control_pipeline::create_audio_source(const std::wstring& id, bool capture)
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

    source_audio_t audio_source;
    audio_source.first.reset(new source_loopback(this->audio_session));
    audio_source.second.reset(new transform_audioprocessor(this->audio_session));
    // audio processor must be initialized before starting/initializing the audio device
    audio_source.second->initialize(audio_source.first.get());
    audio_source.first->initialize(id, capture);
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
    transform_audiomix_t transform_audio_mixer(new transform_audiomix(this->audio_session));
    transform_audio_mixer->initialize();
    return transform_audio_mixer;
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
        video_topology->connect_streams(videoprocessor_stream, preview_stream);
        video_topology->connect_streams(videoprocessor_stream, worker_stream);
    }
    else
    {
        media_stream_t encoder_stream = this->h264_encoder_transform->create_stream();
        media_stream_t color_converter_stream = this->color_converter_transform->create_stream();

        // TODO: encoder stream is redundant
        mpeg_sink_stream->encoder_stream = std::dynamic_pointer_cast<stream_h264_encoder>(encoder_stream);

        video_topology->connect_streams(videoprocessor_stream, color_converter_stream);
        video_topology->connect_streams(videoprocessor_stream, preview_stream);
        video_topology->connect_streams(color_converter_stream, encoder_stream);
        video_topology->connect_streams(encoder_stream, worker_stream);
    }

    video_topology->connect_streams(worker_stream, mpeg_sink_stream);
}

void control_pipeline::build_audio_topology_branch(const media_topology_t& audio_topology,
    const media_stream_t& last_stream,
    const stream_audio_t& audio_sink_stream)
{
    stream_audio_worker_t worker_stream = this->audio_sink.second->create_worker_stream();

    audio_sink_stream->add_worker_stream(worker_stream);

    if(this->item_mpeg_sink.null_file)
    {
        audio_topology->connect_streams(last_stream, worker_stream);
    }
    else
    {
        media_stream_t encoder_stream = this->aac_encoder_transform->create_stream();

        audio_topology->connect_streams(last_stream, encoder_stream);
        audio_topology->connect_streams(encoder_stream, worker_stream);
    }

    audio_topology->connect_streams(worker_stream, audio_sink_stream);
}

void control_pipeline::set_active(control_scene& scene)
{
    const bool first_start = !this->scene_active;

    this->activate_components();

    scene.activate_scene();
    if(this->scene_active && this->scene_active != &scene)
        this->scene_active->deactivate_scene();
    this->scene_active = &scene;

    if(first_start)
    {
#ifdef _DEBUG
        static bool once_ = false;
        if(!once_)
            once_ = true;
        else
            assert_(false);
#endif
        // start the media session with the topology
        // it's ok to start with time point of 0 because the starting happens only once
        // in the lifetime of pipeline;
        this->session->start_playback(this->scene_active->video_topology, 0);
    }
    else
    {
        this->session->switch_topology(this->scene_active->video_topology);
    }
}

void control_pipeline::set_inactive()
{
    assert_(this->scene_active);

    this->scene_active->deactivate_scene();
    this->scene_active = NULL;
    this->session->stop_playback();
    this->session->shutdown();
    this->audio_session->stop_playback();
    this->audio_session->shutdown();
    this->deactivate_components();
    
    // TODO: the pipeline might throw when the media foundation has already been
    // closed while the pipeline is still active;
    // async callback should just ignore the exceptions
}

void control_pipeline::start_recording(const std::wstring& filename, control_scene& scene)
{
    mpeg_sink_item item;
    item.null_file = false;
    item.filename = filename;
    this->set_mpeg_sink_item(item);

    this->set_active(scene);
}

void control_pipeline::stop_recording()
{
    assert_(!this->item_mpeg_sink.null_file);
    assert_(this->scene_active);

    mpeg_sink_item item;
    item.null_file = true;
    this->set_mpeg_sink_item(item);

    this->set_active(*this->scene_active);

    WaitForSingleObject(this->stopped_signal, INFINITE);
    ResetEvent(this->stopped_signal);
}