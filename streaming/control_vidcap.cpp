#include "control_vidcap.h"
#include "control_pipeline.h"

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

control_vidcap::control_vidcap(control_set_t& active_controls,
    control_pipeline& pipeline) :
    control_video(active_controls, pipeline),
    pipeline(pipeline),
    videomixer_params(new stream_videomixer_controller),
    reference(NULL)
{
    this->apply_default_video_params();
}

void control_vidcap::build_video_topology(const media_stream_t& from,
    const media_stream_t& to, const media_topology_t& topology)
{
    if(!this->component)
        return;

    assert_(!this->disabled);

    stream_videomixer_base_t videomixer_stream = 
        std::dynamic_pointer_cast<stream_videomixer_base>(to);
    if(!videomixer_stream)
        throw HR_EXCEPTION(E_UNEXPECTED);

    if(!this->reference)
    {
        media_stream_t vidcap_stream = this->component->create_stream(topology->get_message_generator());

        vidcap_stream->connect_streams(from, topology);
        videomixer_stream->connect_streams(vidcap_stream, this->videomixer_params, topology);

        this->stream = vidcap_stream;
    }
    else
    {
        assert_(this->reference->stream);
        assert_(!this->stream);

        // only connect from this stream to 'to' stream
        // (since this a duplicate control from the original)
        videomixer_stream->connect_streams(
            this->reference->stream, this->videomixer_params, topology);
    }
}

void control_vidcap::activate(const control_set_t& last_set, control_set_t& new_set)
{
    source_vidcap_t component;

    this->stream = NULL;
    this->reference = NULL;

    if(this->disabled)
        goto out;

    for(auto&& control : new_set)
    {
        if(this->is_identical_control(control))
        {
            const control_vidcap* vidcap_control = (const control_vidcap*)control.get();
            this->reference = vidcap_control;
            component = vidcap_control->component;

            break;
        }
    }

    if(!component)
    {
        for(auto&& control : last_set)
        {
            if(this->is_identical_control(control))
            {
                const control_vidcap* vidcap_control = (const control_vidcap*)control.get();
                component = vidcap_control->component;

                break;
            }
        }

        if(!component)
        {
            source_vidcap_t vidcap_source(
                new source_vidcap(this->pipeline.session, this->pipeline.context_mutex));
            vidcap_source->initialize(this->pipeline.shared_from_this<control_pipeline>(),
                this->pipeline.d3d11dev,
                this->pipeline.devctx,
                this->params.symbolic_link);

            component = vidcap_source;
        }
    }

    new_set.push_back(this->shared_from_this<control_vidcap>());

out:
    this->component = component;

    if(this->component)
    {
        // update the transformations when the new control_vidcap activates;
        // this allows components to reactivate the active scene and update their native size
        this->control_video::apply_transformation(false);
        this->control_video::apply_transformation(true);

        this->event_provider.for_each([this](gui_event_handler* e) { e->on_activate(this, false); });
    }
    else
        this->event_provider.for_each([this](gui_event_handler* e) { e->on_activate(this, true); });
}

void control_vidcap::list_available_vidcap_params(
    const control_pipeline_t& /*pipeline*/,
    std::vector<vidcap_params>& params)
{
    assert_(params.empty());

    HRESULT hr = S_OK;
    CComPtr<IMFAttributes> attributes;
    IMFActivate** devices = NULL;

    CHECK_HR(hr = MFCreateAttributes(&attributes, 1));
    CHECK_HR(hr = attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));

    UINT32 count;
    CHECK_HR(hr = MFEnumDeviceSources(attributes, &devices, &count));
    for(UINT32 i = 0; i < count; i++)
    {
        vidcap_params param;

        WCHAR* friendly_name = NULL, *symbolic_link = NULL;
        UINT32 len;
        CHECK_HR(hr = devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
            &friendly_name, &len));
        param.friendly_name = friendly_name;
        CoTaskMemFree(friendly_name);

        CHECK_HR(hr = devices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            &symbolic_link, &len));
        param.symbolic_link = symbolic_link;
        CoTaskMemFree(symbolic_link);

        params.push_back(std::move(param));
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

D2D1_RECT_F control_vidcap::get_rectangle(bool /*dest_params*/) const
{
    const source_vidcap_t& component = this->reference ?
        this->reference->component : this->component;

    if(component)
    {
        UINT32 width, height;
        component->get_size(width, height);

        return D2D1::RectF(0.f, 0.f, (FLOAT)width, (FLOAT)height);
    }
    else
        return D2D1::RectF();
}

void control_vidcap::apply_transformation(
    const D2D1::Matrix3x2F&& transformation, bool dest_params)
{
    const D2D1_RECT_F rect = this->get_rectangle(dest_params);
    stream_videomixer_controller::params_t params;

    this->videomixer_params->get_params(params);

    if(dest_params)
    {
        const video_params_t video_params = this->get_video_params(dest_params);
        params.dest_rect = rect;
        params.dest_m = transformation;
        params.axis_aligned_clip = ((video_params.rotate / 90.f) ==
            std::round(video_params.rotate / 90.f));
    }
    else
    {
        params.source_rect = rect;
        params.source_m = transformation;
    }

    this->videomixer_params->set_params(params);
}

void control_vidcap::set_default_video_params(video_params_t& video_params, bool dest_params)
{
    video_params.rotate = dest_params ? 0.f : 0.f;
    video_params.translate = dest_params ? D2D1::Point2F(100.f, 100.f) : D2D1::Point2F();
    video_params.scale = D2D1::Point2F(1.f, 1.f);
}

bool control_vidcap::is_identical_control(const control_class_t& control) const
{
    const control_vidcap* vidcap_control = dynamic_cast<const control_vidcap*>(control.get());

    if(!vidcap_control || !vidcap_control->component)
        return false;

    if(vidcap_control->component->get_instance_type() == media_component::INSTANCE_NOT_SHAREABLE)
        return false;

    return (vidcap_control->params.symbolic_link == this->params.symbolic_link);
}