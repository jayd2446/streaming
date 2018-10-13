#include "control_pipeline2.h"
#include <iostream>
#include <d3d11_4.h>

#ifdef _DEBUG
#define CREATE_DEVICE_DEBUG D3D11_CREATE_DEVICE_DEBUG
#else
#define CREATE_DEVICE_DEBUG 0
#endif

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

control_pipeline2::control_pipeline2() :
    control_class(controls),
    d3d11dev_adapter(0),
    context_mutex(new std::recursive_mutex),
    root_scene(controls, *this),
    recording(false),
    running(false)
{
    this->root_scene.parent = this;

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
    // accidentally corrupted;
    // amd h264 encoder probably caused encoding artifacts because the
    // context was being corrupted
    CHECK_HR(hr = this->devctx->QueryInterface(&multithread));
    was_protected = multithread->SetMultithreadProtected(TRUE);

    std::cout << "adapter " << this->d3d11dev_adapter << std::endl;

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void control_pipeline2::activate(const control_set_t& last_set, control_set_t& new_set)
{
    if(this->disabled)
    {
        this->root_scene.disabled = true;
        this->root_scene.activate(last_set, new_set);
        this->deactivate_components();

        return;
    }

    this->activate_components();

    // add this to the new set
    new_set.push_back(this);

    // activate the root scene
    this->root_scene.activate(last_set, new_set);
}

void control_pipeline2::activate_components()
{
    /*this->running = true;*/

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

    // create videoprocessor transform
    if(!this->videoprocessor_transform ||
        this->videoprocessor_transform->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE)
    {
        transform_videoprocessor_t videoprocessor_transform(
            new transform_videoprocessor(this->session, this->context_mutex));
        videoprocessor_transform->initialize(this->shared_from_this<control_pipeline2>(),
            this->d3d11dev, this->devctx);

        this->videoprocessor_transform = videoprocessor_transform;
    }

    // create h264 transform
    if(this->recording && (!this->h264_encoder_transform ||
        this->h264_encoder_transform->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE))
    {
        // TODO: activating the encoder might fail for random reasons,
        // so notify if the primary encoder cannot be used and use the software encoder as a
        // fallback
        // (signature error during activate call was fixed by a reboot)
        transform_h264_encoder_t h264_encoder_transform;
        try
        {
            h264_encoder_transform.reset(new transform_h264_encoder(
                this->session, this->context_mutex));
            h264_encoder_transform->initialize(this->shared_from_this<control_pipeline2>(),
                this->d3d11dev, false);
        }
        catch(std::exception)
        {
            std::cout << "using system ram for hardware video encoder" << std::endl;

            try
            {
                // try to initialize the h264 encoder without utilizing vram
                h264_encoder_transform.reset(new transform_h264_encoder(
                    this->session, this->context_mutex));
                h264_encoder_transform->initialize(this->shared_from_this<control_pipeline2>(),
                    NULL);
            }
            catch(std::exception)
            {
                std::cout << "using software encoder" << std::endl;
                // use software encoder
                h264_encoder_transform.reset(new transform_h264_encoder(
                    this->session, this->context_mutex));
                h264_encoder_transform->initialize(this->shared_from_this<control_pipeline2>(),
                    NULL, true);
            }
        }

        this->h264_encoder_transform = h264_encoder_transform;
    }
    else if(!this->recording)
        this->h264_encoder_transform = NULL;

    // create color converter transform
    if(this->recording && (!this->color_converter_transform ||
        this->color_converter_transform->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE))
    {
        transform_color_converter_t color_converter_transform(
            new transform_color_converter(this->session, this->context_mutex));
        color_converter_transform->initialize(this->shared_from_this<control_pipeline2>(),
            this->d3d11dev, this->devctx);
        this->color_converter_transform = color_converter_transform;
    }
    else if(!this->recording)
        this->color_converter_transform = NULL;

    // create preview window sink
    if(!this->preview_sink || 
        this->preview_sink->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE)
    {
        sink_preview2_t preview_sink(new sink_preview2(this->session, this->context_mutex));
        preview_sink->initialize(this->preview_hwnd, this->d3d11dev);

        this->preview_sink = preview_sink;
    }

    // create aac encoder transform
    if(this->recording && (!this->aac_encoder_transform ||
        this->aac_encoder_transform->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE))
    {
        transform_aac_encoder_t aac_encoder_transform(new transform_aac_encoder(this->audio_session));
        aac_encoder_transform->initialize();

        this->aac_encoder_transform = aac_encoder_transform;
    }
    else if(!this->recording)
        this->aac_encoder_transform = NULL;

    // create audiomixer transform
    if(!this->audiomixer_transform || 
        this->audiomixer_transform->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE)
    {
        transform_audiomixer_t audiomixer_transform(new transform_audiomixer(this->audio_session));
        audiomixer_transform->initialize();

        this->audiomixer_transform = audiomixer_transform;
    }

    output_file_t file_output;

    // create mp4 file sink video part
    if(this->recording && (!this->mp4_sink.first ||
        this->mp4_sink.first->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE))
    {
        sink_file_t file_sink(new sink_file(this->session));
        if(!file_output)
        {
            file_output.reset(new output_file);
            file_output->initialize(false, this->stopped_signal,
                this->h264_encoder_transform->output_type,
                this->aac_encoder_transform->output_type);
        }

        file_sink->initialize(file_output, true);

        this->mp4_sink.first = file_sink;
    }
    else if(!this->recording)
        this->mp4_sink.first = NULL;

    // create mp4 file sink audio part
    if(this->recording && (!this->mp4_sink.second ||
        this->mp4_sink.second->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE))
    {
        sink_file_t file_sink(new sink_file(this->audio_session));
        if(!file_output)
        {
            file_output.reset(new output_file);
            file_output->initialize(false, this->stopped_signal,
                this->h264_encoder_transform->output_type,
                this->aac_encoder_transform->output_type);
        }

        file_sink->initialize(file_output, false);

        this->mp4_sink.second = file_sink;
    }
    else if(!this->recording)
        this->mp4_sink.second = NULL;

    // create mpeg sink(the main/real pull sink)
    if(!this->mpeg_sink)
    {
        sink_mpeg2_t mpeg_sink(new sink_mpeg2(this->session, this->audio_session));
        mpeg_sink->initialize();

        this->mpeg_sink = mpeg_sink;
    }

    // create audio sink(controlled by mpeg sink)
    if(!this->audio_sink)
    {
        sink_audio_t audio_sink(new sink_audio(this->audio_session));
        audio_sink->initialize();

        this->audio_sink = audio_sink;
    }
}

void control_pipeline2::deactivate_components()
{
    this->running = false;

    // stop the playback by switching to empty topologies
    this->video_topology.reset(new media_topology(this->time_source));
    this->audio_topology.reset(new media_topology(this->time_source));
    this->mpeg_sink->switch_topologies(this->video_topology, this->audio_topology);

    this->videoprocessor_transform = NULL;
    this->h264_encoder_transform = NULL;
    this->color_converter_transform = NULL;
    this->preview_sink = NULL;
    this->mp4_sink = sink_mp4_t(NULL, NULL);
    this->mpeg_sink = NULL;
    this->aac_encoder_transform = NULL;
    this->audiomixer_transform = NULL;
    this->audio_sink = NULL;

    // shutdown specific
    /*this->video_topology = NULL;
    this->audio_topology = NULL;

    this->session->stop_playback();
    this->session->shutdown();
    this->audio_session->stop_playback();
    this->audio_session->shutdown();*/

    this->session = NULL;
    this->audio_session = NULL;
    this->time_source = NULL;

    /*Sleep(INFINITE);*/
}

void control_pipeline2::build_and_switch_topology()
{
    if(this->disabled)
        return;

    this->video_topology.reset(new media_topology(this->time_source));
    this->audio_topology.reset(new media_topology(this->time_source));

    stream_audio_t audio_stream = this->audio_sink->create_stream(this->audio_topology->get_clock());
    stream_mpeg2_t mpeg_stream = this->mpeg_sink->create_stream(
        this->video_topology->get_clock(), audio_stream);

    mpeg_stream->set_pull_rate(
        transform_h264_encoder::frame_rate_num, transform_h264_encoder::frame_rate_den);

    for(int i = 0; i < WORKER_STREAMS; i++)
    {
        stream_mpeg2_worker_t mpeg_worker_stream = this->mpeg_sink->create_worker_stream();
        media_stream_t preview_stream = this->preview_sink->create_stream();
        stream_audio_worker_t audio_worker_stream = this->audio_sink->create_worker_stream();
        media_stream_t audiomixer_stream = this->audiomixer_transform->create_stream(
            this->audio_topology->get_clock());
        stream_videoprocessor_t videoprocessor_stream = 
            this->videoprocessor_transform->create_stream();

        mpeg_stream->add_worker_stream(mpeg_worker_stream);
        audio_stream->add_worker_stream(audio_worker_stream);

        this->root_scene.build_video_topology_branch(videoprocessor_stream, this->video_topology);
        this->root_scene.build_audio_topology_branch(audiomixer_stream, this->audio_topology);

        preview_stream->connect_streams(videoprocessor_stream, this->video_topology);

        if(!this->recording)
        {
            mpeg_worker_stream->connect_streams(preview_stream, this->video_topology);
            audio_worker_stream->connect_streams(audiomixer_stream, this->audio_topology);
        }
        else
        {
            media_stream_t encoder_stream_video =
                this->h264_encoder_transform->create_stream(this->video_topology->get_clock());
            media_stream_t color_converter_stream = this->color_converter_transform->create_stream();
            media_stream_t mp4_stream_video = 
                this->mp4_sink.first->create_stream(this->video_topology->get_clock());
            media_stream_t encoder_stream_audio =
                this->aac_encoder_transform->create_stream(this->audio_topology->get_clock());
            media_stream_t mp4_stream_audio = 
                this->mp4_sink.second->create_stream(this->audio_topology->get_clock());

            // TODO: encoder stream is redundant
            mpeg_stream->encoder_stream = 
                std::dynamic_pointer_cast<stream_h264_encoder>(encoder_stream_video);

            color_converter_stream->connect_streams(preview_stream, this->video_topology);
            encoder_stream_video->connect_streams(color_converter_stream, this->video_topology);
            mp4_stream_video->connect_streams(encoder_stream_video, this->video_topology);
            mpeg_worker_stream->connect_streams(mp4_stream_video, this->video_topology);

            encoder_stream_audio->connect_streams(audiomixer_stream, this->audio_topology);
            mp4_stream_audio->connect_streams(encoder_stream_audio, this->audio_topology);
            audio_worker_stream->connect_streams(mp4_stream_audio, this->audio_topology);
        }

        mpeg_stream->connect_streams(mpeg_worker_stream, this->video_topology);
        audio_stream->connect_streams(audio_worker_stream, this->audio_topology);
    }

    // mpeg sink ensures atomic topology starting/switching for audio and video
    if(!this->running/*this->session->started()*/)
    {
        this->running = true;
        // start the media session with the topology;
        // it's ok to start with time point of 0 because the time source starts at 0
        this->mpeg_sink->start_topologies(0, this->video_topology, this->audio_topology);
    }
    else
        this->mpeg_sink->switch_topologies(this->video_topology, this->audio_topology);
}

CHandle control_pipeline2::start_recording(const std::wstring& /*filename*/)
{
    assert_(!this->is_recording());
    assert_(!this->stopped_signal);
    assert_(!this->is_disabled());

    this->stopped_signal.Attach(CreateEvent(NULL, TRUE, FALSE, NULL));
    if(!this->stopped_signal)
        throw HR_EXCEPTION(E_UNEXPECTED);

    this->recording = true;
    control_class::activate();

    return CHandle(this->stopped_signal);
}

void control_pipeline2::stop_recording()
{
    this->recording = false;
    control_class::activate();
}