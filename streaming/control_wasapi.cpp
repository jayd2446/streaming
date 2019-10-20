#include "control_wasapi.h"
#include "control_pipeline.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

control_wasapi::control_wasapi(control_set_t& active_controls, control_pipeline& pipeline) :
    control_class(active_controls, pipeline.event_provider),
    audiomixer_params(new stream_audiomixer2_controller),
    pipeline(pipeline),
    reference(NULL)
{
}

void control_wasapi::build_audio_topology(const media_stream_t& from,
    const media_stream_t& to, const media_topology_t& topology)
{
    if(!this->component)
        return;

    assert_(!this->disabled);

    stream_audiomixer2_base_t audiomixer_stream =
        std::dynamic_pointer_cast<stream_audiomixer2_base>(to);
    if(!audiomixer_stream)
        throw HR_EXCEPTION(E_UNEXPECTED);

    if(!this->reference)
    {
        media_stream_t wasapi_stream = this->component->create_stream(topology->get_message_generator());

        wasapi_stream->connect_streams(from, topology);
        audiomixer_stream->connect_streams(wasapi_stream, this->audiomixer_params, topology);

        this->stream = wasapi_stream;
    }
    else
    {
        // the topology branch for the referenced control must have been established beforehand
        assert_(this->reference->stream);
        assert_(!this->stream);

        // only connect from this stream to 'to' stream
        // (since this a duplicate control from the original)
        audiomixer_stream->connect_streams(
            this->reference->stream, this->audiomixer_params, topology);
    }
}

void control_wasapi::activate(const control_set_t& last_set, control_set_t& new_set)
{
    source_wasapi_t component;

    this->stream = NULL;
    this->reference = NULL;

    if(this->disabled)
        goto out;

    // try to find a control to reference in the new set
    for(auto&& control : new_set)
    {
        if(this->is_identical_control(control))
        {
            const control_wasapi* wasapi_control = (const control_wasapi*)control.get();
            this->reference = wasapi_control;
            component = wasapi_control->component;

            break;
        }
    }

    if(!component)
    {
        // try to reuse the component stored in the last set's control
        for(auto&& control : last_set)
        {
            if(this->is_identical_control(control))
            {
                const control_wasapi* wasapi_control = (const control_wasapi*)control.get();
                component = wasapi_control->component;

                break;
            }
        }

        if(!component)
        {
            // create a new component since it was not found in the last or in the new set
            source_wasapi_t wasapi_source(new source_wasapi(this->pipeline.audio_session));

            wasapi_source->initialize(this->pipeline.shared_from_this<control_pipeline>(),
                this->params.device_id, this->params.capture);

            component = wasapi_source;
        }
    }

    new_set.push_back(this->shared_from_this<control_wasapi>());

out:
    this->component = component;

    if(this->component)
        this->event_provider.for_each([this](gui_event_handler* e) { e->on_activate(this, false); });
    else
        this->event_provider.for_each([this](gui_event_handler* e) { e->on_activate(this, true); });
}

void control_wasapi::list_available_wasapi_params(
    std::vector<wasapi_params>& wasapis)
{
    auto f = [&](EDataFlow flow)
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
            wasapi_params params;
            CComPtr<IMMDevice> device;
            CComPtr<IPropertyStore> props;
            PROPVARIANT name;
            LPWSTR id;
            PropVariantInit(&name);

            CHECK_HR(hr = collection->Item(i, &device));
            CHECK_HR(hr = device->GetId(&id));
            CHECK_HR(hr = device->OpenPropertyStore(STGM_READ, &props));
            CHECK_HR(hr = props->GetValue(PKEY_Device_FriendlyName, &name));

            params.device_friendlyname = name.pwszVal;
            params.device_id = id;
            params.capture = (flow == eCapture);

            CoTaskMemFree(id);
            PropVariantClear(&name);
            id = NULL;

            wasapis.push_back(params);
        }

    done:
        // TODO: memory is leaked if this fails
        if(FAILED(hr))
            throw HR_EXCEPTION(hr);
    };

    f(eCapture);
    f(eRender);
}

bool control_wasapi::is_identical_control(const control_class_t& control) const
{
    const control_wasapi* wasapi_control = dynamic_cast<const control_wasapi*>(control.get());

    // check that the control is of wasapi type and it stores a component
    if(!wasapi_control || !wasapi_control->component)
        return false;

    // check that the component isn't requesting a reinitialization
    if(wasapi_control->component->get_instance_type() ==
        media_component::INSTANCE_NOT_SHAREABLE)
        return false;

    // check that the component will reside in the same session
    if(wasapi_control->component->session != this->pipeline.audio_session)
        return false;

    // check that the control params match this control's params
    return (wasapi_control->params.device_id == this->params.device_id &&
        wasapi_control->params.capture == this->params.capture);
}