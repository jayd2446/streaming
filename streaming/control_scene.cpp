#include "control_scene.h"
#include "control_pipeline.h"
#include "assert.h"
#include <functiondiscoverykeys_devpkey.h>

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

    // TODO: support for multiple displaycapture and audio sources via mixer transforms
    // TODO: loopback source param in audio sink is redundant
    // TODO: fps num and den in pipeline

    // create streams
    stream_mpeg2_t mpeg_stream = this->pipeline.mpeg_sink->create_stream(this->video_topology->get_clock());
    stream_audio_t audio_stream = this->pipeline.audio_sink->create_stream(
        this->audio_topology->get_clock(), this->audio_sources[0].second);

    this->pipeline.mpeg_sink->set_new_audio_topology(audio_stream, this->audio_topology);
    mpeg_stream->set_pull_rate(60, 1);

    for(int i = 0; i < WORKER_STREAMS; i++)
    {
        int displaycapture_index = 0;

        // video
        {
            stream_mpeg2_worker_t worker_stream = this->pipeline.mpeg_sink->create_worker_stream();
            media_stream_t encoder_stream = this->pipeline.h264_encoder_transform->create_stream();
            media_stream_t color_converter_stream = this->pipeline.color_converter_transform->create_stream();
            media_stream_t preview_stream = this->pipeline.preview_sink->create_stream();

            // TODO: encoder stream is redundant
            mpeg_stream->add_worker_stream(worker_stream);
            mpeg_stream->encoder_stream = std::dynamic_pointer_cast<stream_h264_encoder>(encoder_stream);

            // send the source input streams to videoprocessor transform
            stream_videoprocessor_t videoprocessor_stream = 
                this->pipeline.videoprocessor_transform->create_stream();
            for(size_t i = 0; i < this->video_items.size(); i++)
            {
                switch(this->video_items[i].type)
                {
                case DISPLAYCAPTURE_ITEM:
                    {
                        media_stream_t displaycapture_stream = 
                            this->displaycapture_sources[displaycapture_index].second->create_stream(
                            this->videoprocessor_stream_controllers[i]);
                        videoprocessor_stream->add_input_stream(displaycapture_stream.get());
                        this->video_topology->connect_streams(displaycapture_stream, videoprocessor_stream);
                    }
                    displaycapture_index++;
                    break;
                default:
                    throw std::exception();
                }
            }

            // connect the video processor stream to the rest of the streams
            this->video_topology->connect_streams(videoprocessor_stream, color_converter_stream);
            this->video_topology->connect_streams(videoprocessor_stream, preview_stream);
            this->video_topology->connect_streams(color_converter_stream, encoder_stream);
            this->video_topology->connect_streams(encoder_stream, worker_stream);
            this->video_topology->connect_streams(worker_stream, mpeg_stream);
        }

        // audio
        {
            stream_audio_worker_t worker_stream = this->pipeline.audio_sink->create_worker_stream();
            media_stream_t aac_encoder_stream = this->pipeline.aac_encoder_transform->create_stream();

            audio_stream->add_worker_stream(worker_stream);

            // chain first audio source to processor
            media_stream_t first_audio_stream = this->audio_sources[0].second->create_stream();
            media_stream_t last_stream = this->audio_processors[0]->create_stream();
            this->audio_topology->connect_streams(first_audio_stream, last_stream);

            // chain audio mixers
            for(size_t i = 1; i < this->audio_items.size(); i++)
            {
                stream_audiomix_t audiomix_stream = this->audio_mixers[i - 1]->create_stream();
                // no need for switch-case because currently all audio items are of loopback source type
                media_stream_t audio_stream = this->audio_sources[i].second->create_stream();
                media_stream_t audioprocessor_stream = this->audio_processors[i]->create_stream();

                audiomix_stream->set_primary_stream(last_stream.get());

                this->audio_topology->connect_streams(last_stream, audiomix_stream);
                this->audio_topology->connect_streams(audio_stream, audioprocessor_stream);
                this->audio_topology->connect_streams(audioprocessor_stream, audiomix_stream);

                last_stream = audiomix_stream;
            }

            this->audio_topology->connect_streams(last_stream, aac_encoder_stream);
            this->audio_topology->connect_streams(aac_encoder_stream, worker_stream);
            this->audio_topology->connect_streams(worker_stream, audio_stream);
        }
    }
}

void control_scene::activate_scene()
{
    assert_(this->displaycapture_sources.empty() && 
        this->audio_sources.empty() &&
        this->videoprocessor_stream_controllers.empty() &&
        this->audio_mixers.empty() &&
        this->audio_processors.empty());

    // activate displaycapture items
    for(auto it = this->displaycapture_items.begin(); it != this->displaycapture_items.end(); it++)
    {
        source_displaycapture5_t displaycapture_source = this->pipeline.create_displaycapture_source(
            it->adapter_ordinal, it->output_ordinal);
        this->displaycapture_sources.push_back(std::make_pair(*it, displaycapture_source));
    }

    // activate video processor stream controllers
    for(auto it = this->video_items.begin(); it != this->video_items.end(); it++)
    {
        stream_videoprocessor_controller_t controller(new stream_videoprocessor_controller);
        stream_videoprocessor_controller::params_t params;
        params.source_rect = it->source_rect;
        params.dest_rect = it->dest_rect;
        controller->set_params(params);

        this->videoprocessor_stream_controllers.push_back(controller);
    }

    // activate audio items
    for(auto it = this->audio_items.begin(); it != this->audio_items.end(); it++)
    {
        source_loopback_t loopback_source = this->pipeline.create_audio_source(
            it->device_id, it->capture);
        this->audio_sources.push_back(std::make_pair(*it, loopback_source));
    }

    // activate audio processors
    for(auto it = this->audio_items.begin(); it != this->audio_items.end(); it++)
        this->audio_processors.push_back(this->pipeline.create_audio_processor());

    // activate audio mixers
    for(int i = 0; i < (int)this->audio_items.size() - 1; i++)
        this->audio_mixers.push_back(this->pipeline.create_audio_mixer());

    // reset the topologies
    this->reset_topology(true);
}

void control_scene::deactivate_scene()
{
    this->displaycapture_sources.clear();
    this->audio_sources.clear();
    this->videoprocessor_stream_controllers.clear();
    this->audio_mixers.clear();
    this->audio_processors.clear();

    // reset the topologies
    this->reset_topology(false);
}

void control_scene::add_displaycapture_item(const displaycapture_item& item)
{
    if(item.video.type != DISPLAYCAPTURE_ITEM)
        throw std::exception();

    this->video_items.push_back(item.video);
    this->displaycapture_items.push_back(item);
}

void control_scene::add_audio_item(const audio_item& audio)
{
    this->audio_items.push_back(audio);
}