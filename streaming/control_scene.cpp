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
        // video
        {
            stream_mpeg2_worker_t worker_stream = this->pipeline.mpeg_sink->create_worker_stream();
            media_stream_t encoder_stream = this->pipeline.h264_encoder_transform->create_stream();
            media_stream_t color_converter_stream = this->pipeline.color_converter_transform->create_stream();
            media_stream_t displaycapture_stream = this->displaycapture_sources[0].second->create_stream();
            media_stream_t preview_stream = this->pipeline.preview_sink->create_stream();

            // TODO: encoder stream is redundant
            mpeg_stream->add_worker_stream(worker_stream);
            mpeg_stream->encoder_stream = std::dynamic_pointer_cast<stream_h264_encoder>(encoder_stream);

            this->video_topology->connect_streams(displaycapture_stream, color_converter_stream);
            this->video_topology->connect_streams(displaycapture_stream, preview_stream);
            this->video_topology->connect_streams(color_converter_stream, encoder_stream);
            this->video_topology->connect_streams(encoder_stream, worker_stream);
            this->video_topology->connect_streams(worker_stream, mpeg_stream);
        }

        // audio
        {
            stream_audio_worker_t worker_stream = this->pipeline.audio_sink->create_worker_stream();
            media_stream_t aac_encoder_stream = this->pipeline.aac_encoder_transform->create_stream();
            media_stream_t loopback_stream = this->audio_sources[0].second->create_stream();

            audio_stream->add_worker_stream(worker_stream);

            this->audio_topology->connect_streams(loopback_stream, aac_encoder_stream);
            this->audio_topology->connect_streams(aac_encoder_stream, worker_stream);
            this->audio_topology->connect_streams(worker_stream, audio_stream);
        }
    }
}

void control_scene::activate_scene()
{
    assert_(this->displaycapture_sources.empty() && this->audio_sources.empty());

    // activate displaycapture items
    for(auto it = this->displaycapture_items.begin(); it != this->displaycapture_items.end(); it++)
    {
        source_displaycapture5_t displaycapture_source = this->pipeline.create_displaycapture_source(
            it->adapter_ordinal, it->output_ordinal);
        this->displaycapture_sources.push_back(std::make_pair(*it, displaycapture_source));
    }

    // activate audio items
    for(auto it = this->audio_items.begin(); it != this->audio_items.end(); it++)
    {
        source_loopback_t loopback_source = this->pipeline.create_audio_source(
            it->device_id, it->capture);
        this->audio_sources.push_back(std::make_pair(*it, loopback_source));
    }

    // reset the topologies
    this->reset_topology(true);
}

void control_scene::deactivate_scene()
{
    // deactivate displaycapture items
    this->displaycapture_sources.clear();
    // deactivate audio items
    this->audio_sources.clear();

    // reset the topologies
    this->reset_topology(false);
}

void control_scene::add_displaycapture_item(const displaycapture_item& item)
{
    this->displaycapture_items.push_back(item);
}

void control_scene::add_audio_item(const audio_item& audio)
{
    this->audio_items.push_back(audio);
}