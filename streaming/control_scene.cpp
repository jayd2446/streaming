#include "control_scene.h"
#include "control_pipeline.h"
#include "source_null.h"
#include "transform_h264_encoder.h"
#include "assert.h"
#include <functiondiscoverykeys_devpkey.h>
#include <queue>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

control_scene::control_scene(control_pipeline& pipeline) : pipeline(pipeline)
{
}

bool control_scene::list_available_displaycapture_items(std::vector<displaycapture_item>& displaycaptures)
{
    assert_(displaycaptures.empty());

    HRESULT hr = S_OK;
    CComPtr<IDXGIAdapter1> adapter;
    for(UINT i = 0; SUCCEEDED(hr = this->pipeline.dxgifactory->EnumAdapters1(i, &adapter)); i++)
    {
        CComPtr<IDXGIOutput> output;
        for(UINT j = 0; SUCCEEDED(hr = adapter->EnumOutputs(j, &output)); j++)
        {
            displaycapture_item item;
            CHECK_HR(hr = adapter->GetDesc1(&item.adapter));
            CHECK_HR(hr = output->GetDesc(&item.output));
            item.adapter_ordinal = i;
            item.output_ordinal = j;

            displaycaptures.push_back(item);
            output = NULL;
        }

        adapter = NULL;
    }

done:
    if(hr != DXGI_ERROR_NOT_FOUND && FAILED(hr))
        throw std::exception();

    return !displaycaptures.empty();
}

bool control_scene::list_available_audio_items(std::vector<audio_item>& audios, EDataFlow flow)
{
    HRESULT hr = S_OK;

    CComPtr<IMMDeviceEnumerator> enumerator;
    CComPtr<IMMDeviceCollection> collection;
    UINT count;

    CHECK_HR(hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, 
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator));
    CHECK_HR(hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection));
    CHECK_HR(hr = collection->GetCount(&count));

    for(UINT i = 0; i < count; i++)
    {
        audio_item audio;
        CComPtr<IMMDevice> device;
        CComPtr<IPropertyStore> props;
        PROPVARIANT name;
        LPWSTR id;
        PropVariantInit(&name);

        CHECK_HR(hr = collection->Item(i, &device));
        CHECK_HR(hr = device->GetId(&id));
        CHECK_HR(hr = device->OpenPropertyStore(STGM_READ, &props));
        CHECK_HR(hr = props->GetValue(PKEY_Device_FriendlyName, &name));

        audio.device_friendlyname = name.pwszVal;
        audio.device_id = id;
        audio.capture = (flow == eCapture);

        CoTaskMemFree(id);
        PropVariantClear(&name);
        id = NULL;

        audios.push_back(audio);
    }

done:
    // TODO: memory is leaked if this fails
    if(FAILED(hr))
        throw std::exception();

    return !audios.empty();
}

bool control_scene::list_available_audio_items(std::vector<audio_item>& audios)
{
    assert_(audios.empty());

    this->list_available_audio_items(audios, eRender);
    this->list_available_audio_items(audios, eCapture);

    return !audios.empty();
}

//bool control_scene::find_displaycapture_source(
//    const displaycapture_item& item, source_displaycapture5_t& source) const
//{
//    for(auto it = this->displaycapture_sources.begin(); it != this->displaycapture_sources.end(); it++)
//    {
//        if(it->first.video.type == item.video.type &&
//            it->first.adapter_ordinal == item.adapter_ordinal &&
//            it->first.output_ordinal == item.output_ordinal &&
//            !it->first.duplicate)
//        {
//            source = it->second;
//            return true;
//        }
//    }
//
//    return false;
//}

void control_scene::reset_topology(bool create_new)
{
    if(!create_new)
    {
        this->video_topology = NULL;
        this->audio_topology = NULL;
        return;
    }

    this->video_topology.reset(new media_topology(this->pipeline.time_source));
    this->audio_topology.reset(new media_topology(this->pipeline.time_source));

    // create streams
    stream_mpeg2_t mpeg_stream = 
        this->pipeline.mpeg_sink.second->create_stream(this->video_topology->get_clock());
    stream_audio_t audio_stream = 
        this->pipeline.audio_sink.second->create_stream(this->audio_topology->get_clock());

    this->pipeline.mpeg_sink.second->set_new_audio_topology(audio_stream, this->audio_topology);
    mpeg_stream->set_pull_rate(
        transform_h264_encoder::frame_rate_num, transform_h264_encoder::frame_rate_den);

    for(int i = 0; i < WORKER_STREAMS; i++)
    {
        // video
        {
            // TODO: decide if move this struct to scene's displaycapture_sources vector
            struct displaycapture_streams_t
            {
                stream_displaycapture5_t stream;
                stream_displaycapture5_pointer_t pointer_stream;
            };
            std::vector<displaycapture_streams_t> displaycapture_streams;
            displaycapture_streams.reserve(this->video_items.size());

            // send the source input streams to videoprocessor transform
            stream_videoprocessor_t videoprocessor_stream = 
                this->pipeline.videoprocessor_transform->create_stream();

            int displaycapture_index = 0;
            for(size_t i = 0; i < this->video_items.size(); i++)
            {
                switch(this->video_items[i].type)
                {
                case DISPLAYCAPTURE_ITEM:
                    displaycapture_streams.push_back(displaycapture_streams_t());

                    if(this->displaycapture_sources[displaycapture_index].first.reference < 0)
                    {
                        const source_displaycapture5_t source = 
                            this->displaycapture_sources[displaycapture_index].second;

                        // TODO: control scene shouldnt control src rect for displaycapture sample
                        stream_videoprocessor_controller_t 
                            videoprocessor_stream_controller(new stream_videoprocessor_controller);
                        stream_videoprocessor_controller::params_t params;
                        params.source_rect.top = params.source_rect.left = 0;
                        params.source_rect.right = 1920;
                        params.source_rect.bottom = 1080;
                        params.dest_rect = params.source_rect;
                        params.enable_alpha = true;
                        videoprocessor_stream_controller->set_params(params);

                        stream_displaycapture5_t displaycapture_stream = source->create_stream(
                            videoprocessor_stream_controller);
                        stream_displaycapture5_pointer_t displaycapture_pointer_stream = 
                            source->create_pointer_stream();

                        displaycapture_stream->set_pointer_stream(displaycapture_pointer_stream);

                        videoprocessor_stream->add_input_stream(
                            displaycapture_stream.get(), this->videoprocessor_stream_controllers[i]);
                        videoprocessor_stream->add_input_stream(
                            displaycapture_pointer_stream.get(), this->videoprocessor_stream_controllers[i]);

                        this->video_topology->connect_streams(displaycapture_stream, videoprocessor_stream);
                        this->video_topology->connect_streams(displaycapture_pointer_stream, videoprocessor_stream);

                        // add the created streams to displaycapture streams cache so that
                        // references can be resolved
                        displaycapture_streams.back().stream = displaycapture_stream;
                        displaycapture_streams.back().pointer_stream = displaycapture_pointer_stream;
                    }
                    else
                    {
                        const int reference = 
                            this->displaycapture_sources[displaycapture_index].first.reference;

                        // the reference must point to earlier displaycapture_item
                        // for topology building to work
                        if(reference >= displaycapture_streams.size())
                            throw std::exception();
                        
                        /*const source_displaycapture5_t source = 
                            this->displaycapture_sources[reference].second;*/

                        videoprocessor_stream->add_input_stream(
                            displaycapture_streams[reference].stream.get(), 
                            this->videoprocessor_stream_controllers[i]);
                        videoprocessor_stream->add_input_stream(
                            displaycapture_streams[reference].pointer_stream.get(),
                            this->videoprocessor_stream_controllers[i]);

                        this->video_topology->connect_streams(
                            displaycapture_streams[reference].stream, videoprocessor_stream);
                        this->video_topology->connect_streams(
                            displaycapture_streams[reference].pointer_stream, videoprocessor_stream);
                    }

                    displaycapture_index++;
                    break;
                default:
                    throw std::exception();
                }
            }

            // set the pipeline specific part of the topology
            this->pipeline.build_video_topology_branch(
                this->video_topology, videoprocessor_stream, mpeg_stream);
        }

        // audio
        if(!this->audio_sources.empty())
        {
            // TODO: source loopback isn't a component anymore

            // chain first audio source to its audio processor
            media_stream_t first_audio_stream = this->audio_sources[0].second.first->create_stream();
            media_stream_t last_stream = this->audio_sources[0].second.second->create_stream();
            this->audio_topology->connect_streams(first_audio_stream, last_stream);

            // TODO: audio mixers should be chained parallel
            // instead of serially

            // chain audio mixers
            for(size_t i = 1; i < this->audio_items.size(); i++)
            {
                stream_audiomix_t audiomix_stream = this->audio_mixers[i - 1]->create_stream();
                // no need for switch-case because currently all audio items are of loopback source type
                media_stream_t audio_stream = this->audio_sources[i].second.first->create_stream();
                media_stream_t audioprocessor_stream = 
                    this->audio_sources[i].second.second->create_stream();

                audiomix_stream->set_primary_stream(last_stream.get());

                this->audio_topology->connect_streams(last_stream, audiomix_stream);
                this->audio_topology->connect_streams(audio_stream, audioprocessor_stream);
                this->audio_topology->connect_streams(audioprocessor_stream, audiomix_stream);

                last_stream = audiomix_stream;
            }

            // set the pipeline specific part of the topology
            this->pipeline.build_audio_topology_branch(
                this->audio_topology, last_stream, audio_stream);
        }
        else
        {
            source_null_t null_source(new source_null(this->pipeline.audio_session));
            media_stream_t null_stream = null_source->create_stream();
            this->pipeline.build_audio_topology_branch(
                this->audio_topology, null_stream, audio_stream);
        }
    }
}

void control_scene::activate_scene()
{
    /*if(!reactivate)
        assert_(this->displaycapture_sources.empty() && 
            this->audio_sources.empty() &&
            this->videoprocessor_stream_controllers.empty() &&
            this->audio_mixers.empty());*/

    // activate scene cannot directly modify the containers because when updating a scene
    // the old container must be immutable so that the pipeline can properly share
    // the components

    // activate displaycapture items
    std::vector<std::pair<displaycapture_item, source_displaycapture5_t>> displaycapture_sources;
    for(auto it = this->displaycapture_items.begin(); it != this->displaycapture_items.end(); it++)
    {
        if(it->reference < 0)
        {
            source_displaycapture5_t displaycapture_source = this->pipeline.create_displaycapture_source(
                it->adapter_ordinal, it->output_ordinal);
            displaycapture_sources.push_back(std::make_pair(*it, displaycapture_source));
        }
        else
            // null source denotes a duplicate
            displaycapture_sources.push_back(std::make_pair(*it, source_displaycapture5_t()));
    }
    this->displaycapture_sources.swap(displaycapture_sources);

    // activate video processor stream controllers
    std::vector<stream_videoprocessor_controller_t> videoprocessor_stream_controllers;
    for(auto it = this->video_items.begin(); it != this->video_items.end(); it++)
    {
        stream_videoprocessor_controller_t controller(new stream_videoprocessor_controller);
        stream_videoprocessor_controller::params_t params;
        params.source_rect = it->source_rect;
        params.dest_rect = it->dest_rect;
        controller->set_params(params);

        videoprocessor_stream_controllers.push_back(controller);
    }
    this->videoprocessor_stream_controllers.swap(videoprocessor_stream_controllers);

    // activate audio items
    std::vector<std::pair<audio_item, source_audio_t>> audio_sources;
    for(auto it = this->audio_items.begin(); it != this->audio_items.end(); it++)
    {
        source_audio_t audio_source = this->pipeline.create_audio_source(
            it->device_id, it->capture);
        audio_sources.push_back(std::make_pair(*it, audio_source));
    }
    this->audio_sources.swap(audio_sources);

    // activate audio mixers
    std::vector<transform_audiomix_t> audio_mixers;
    for(int i = 0; i < (int)this->audio_items.size() - 1; i++)
        audio_mixers.push_back(this->pipeline.create_audio_mixer());
    this->audio_mixers.swap(audio_mixers);

    // reset the topologies
    this->reset_topology(true);
}

void control_scene::deactivate_scene()
{
    this->displaycapture_sources.clear();
    this->audio_sources.clear();
    this->videoprocessor_stream_controllers.clear();
    this->audio_mixers.clear();

    // reset the topologies
    this->reset_topology(false);
}

void control_scene::add_displaycapture_item(
    const displaycapture_item& item, bool force_new_instance)
{
    if(item.video.type != DISPLAYCAPTURE_ITEM)
        throw std::exception();

    this->video_items.push_back(item.video);
    this->displaycapture_items.push_back(item);

    if(!force_new_instance)
    {
        // try to make the new item reference older item
        int i = this->displaycapture_items.size() - 2;
        for(auto it = this->displaycapture_items.rbegin() + 1; 
            it != this->displaycapture_items.rend(); it++, i--)
        {
            if(it->adapter_ordinal == item.adapter_ordinal && it->output_ordinal == item.output_ordinal
                && it->reference < 0)
            {
                this->displaycapture_items.back().reference = i;
                break;
            }
        }
    }
}

void control_scene::add_audio_item(const audio_item& audio)
{
    this->audio_items.push_back(audio);
}