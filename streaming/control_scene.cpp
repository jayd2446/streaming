#include "control_scene.h"
#include "source_empty.h"
#include "source_displaycapture5.h"
#include "source_wasapi.h"
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

void control_scene::build_topology(bool reset)
{
    if(reset)
    {
        this->video_topology = NULL;
        this->audio_topology = NULL;
        return;
    }

    this->video_topology.reset(new media_topology(this->pipeline.time_source));
    this->audio_topology.reset(new media_topology(this->pipeline.time_source));

    // create streams
    stream_audio_t audio_stream = 
        this->pipeline.audio_sink.second->create_stream(this->audio_topology->get_clock());
    stream_mpeg2_t mpeg_stream = 
        this->pipeline.mpeg_sink.second->create_stream(
        this->video_topology->get_clock(), audio_stream);

    /*this->pipeline.mpeg_sink.second->set_new_audio_topology(audio_stream, this->audio_topology);*/
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

                        stream_displaycapture5_t displaycapture_stream = source->create_stream();
                        stream_displaycapture5_pointer_t displaycapture_pointer_stream = 
                            source->create_pointer_stream();

                        displaycapture_stream->set_pointer_stream(displaycapture_pointer_stream);

                        videoprocessor_stream->add_input_stream(
                            displaycapture_stream.get(), this->videoprocessor_stream_controllers[i]);
                        videoprocessor_stream->add_input_stream(
                            displaycapture_pointer_stream.get(), this->videoprocessor_stream_controllers[i]);

                        videoprocessor_stream->connect_streams(displaycapture_stream, this->video_topology);
                        videoprocessor_stream->connect_streams(displaycapture_pointer_stream, this->video_topology);

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

                        videoprocessor_stream->connect_streams(
                            displaycapture_streams[reference].stream, this->video_topology);
                        videoprocessor_stream->connect_streams(
                            displaycapture_streams[reference].pointer_stream, this->video_topology);
                    }

                    displaycapture_index++;
                    break;
                default:
                    throw std::exception();
                }
            }
            if(this->video_items.empty())
            {
                source_empty_video_t empty_source(new source_empty_video(this->pipeline.audio_session));
                media_stream_t empty_stream = empty_source->create_stream();
                videoprocessor_stream->add_input_stream(empty_stream.get(), NULL);
                videoprocessor_stream->connect_streams(empty_stream, this->video_topology);
            }

            // set the pipeline specific part of the topology
            this->pipeline.build_video_topology_branch(
                this->video_topology, videoprocessor_stream, mpeg_stream);
        }

        {
            std::vector<media_stream_t> audioprocessor_streams;
            audioprocessor_streams.reserve(this->audio_items.size());

            // send the source input streams to audiomixer transform
            media_stream_t audiomixer_stream = 
                this->pipeline.audiomixer_transform->create_stream(this->audio_topology->get_clock());

            // audio
            for(size_t i = 0; i < this->audio_items.size(); i++)
            {
                audioprocessor_streams.push_back(media_stream_t());

                if(this->audio_sources[i].first.reference < 0)
                {
                    media_stream_t wasapi_stream = this->audio_sources[i].second.first->create_stream();
                    media_stream_t audioprocessor_stream = 
                        this->audio_sources[i].second.second->create_stream(
                        this->audio_topology->get_clock());

                    audioprocessor_stream->connect_streams(wasapi_stream, this->audio_topology);
                    audiomixer_stream->connect_streams(audioprocessor_stream, this->audio_topology);

                    // add the created streams to audioprocessor streams cache so that
                    // references can be resolved
                    audioprocessor_streams.back() = audioprocessor_stream;
                }
                else
                {
                    const int reference = this->audio_sources[i].first.reference;

                    if(reference >= audioprocessor_streams.size())
                        throw std::exception();

                    audiomixer_stream->connect_streams(
                        audioprocessor_streams[reference], this->audio_topology);
                }
            }
            // add the silent source so that in case of source_wasapi stopping to send samples
            // the audio will be still recorded
            source_empty_audio_t empty_source(new source_empty_audio(this->pipeline.audio_session));
            media_stream_t empty_stream = empty_source->create_stream();
            audiomixer_stream->connect_streams(empty_stream, this->audio_topology);

            // set the pipeline specific part of the topology
            this->pipeline.build_audio_topology_branch(
                this->audio_topology, audiomixer_stream, audio_stream);
        }
    }
}

void control_scene::activate_scene()
{
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
        if(it->reference < 0)
        {
            source_audio_t audio_source = this->pipeline.create_audio_source(
                it->device_id, it->capture);
            audio_sources.push_back(std::make_pair(*it, audio_source));
        }
        else
            // null source denotes a duplicate
            audio_sources.push_back(std::make_pair(*it, source_audio_t()));
    }
    this->audio_sources.swap(audio_sources);

    // reset the topologies
    this->build_topology(false);
}

void control_scene::deactivate_scene()
{
    this->displaycapture_sources.clear();
    this->audio_sources.clear();
    this->videoprocessor_stream_controllers.clear();

    // reset the topologies
    this->build_topology(true);
}

void control_scene::add_displaycapture_item(const displaycapture_item& item, bool force_new_instance)
{
    if(item.video.type != DISPLAYCAPTURE_ITEM)
        throw std::exception();

    this->video_items.push_back(item.video);
    this->displaycapture_items.push_back(item);

    if(!force_new_instance)
    {
        // try to make the new item reference older item
        int i = (int)this->displaycapture_items.size() - 2;
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

void control_scene::add_audio_item(const audio_item& item, bool force_new_instance)
{
    this->audio_items.push_back(item);

    if(!force_new_instance)
    {
        // try to make the new item reference older item
        int i = (int)this->audio_items.size() - 2;
        for(auto it = this->audio_items.rbegin() + 1;
            it != this->audio_items.rend(); it++, i--)
        {
            if(it->capture == item.capture && it->device_id == item.device_id && it->reference < 0)
            {
                this->audio_items.back().reference = i;
                break;
            }
        }
    }
}