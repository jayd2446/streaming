#include "control_displaycapture5.h"
#include "control_pipeline2.h"

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

control_displaycapture::control_displaycapture(control_set_t& active_controls, 
    control_pipeline2& pipeline) :
    control_video2(active_controls, pipeline),
    videoprocessor_params(new stream_videoprocessor2_controller),
    /*control_class(active_controls, pipeline.mutex),
    pipeline(pipeline),*/
    reference(NULL)
{
}

void control_displaycapture::build_video_topology_branch(
    const media_stream_t& to, const media_topology_t& topology)
{
    if(!this->component)
        return;

    assert_(!this->disabled);

    stream_videoprocessor2_t videoprocessor_stream =
        std::dynamic_pointer_cast<stream_videoprocessor2>(to);
    if(!videoprocessor_stream)
        throw HR_EXCEPTION(E_UNEXPECTED);

    if(!this->reference)
    {
        stream_displaycapture5_t displaycapture_stream = this->component->create_stream();
        stream_displaycapture5_pointer_t displaycapture_pointer_stream = 
            this->component->create_pointer_stream();

        displaycapture_stream->set_pointer_stream(displaycapture_pointer_stream);

        videoprocessor_stream->connect_streams(
            displaycapture_pointer_stream, this->videoprocessor_params, topology);
        videoprocessor_stream->connect_streams(
            displaycapture_stream, this->videoprocessor_params, topology);

        this->stream = displaycapture_stream;
        this->pointer_stream = displaycapture_pointer_stream;
    }
    else
    {
        assert_(this->reference->stream);
        assert_(this->reference->pointer_stream);
        assert_(!this->stream);

        videoprocessor_stream->connect_streams(
            this->reference->pointer_stream, this->videoprocessor_params, topology);
        videoprocessor_stream->connect_streams(
            this->reference->stream, this->videoprocessor_params, topology);
    }
}

void control_displaycapture::activate(const control_set_t& last_set, control_set_t& new_set)
{
    source_displaycapture5_t component;

    this->stream = NULL;
    this->pointer_stream = NULL;
    this->reference = NULL;

    if(this->disabled)
        goto out;

    // try to find a control to reference in the new set
    std::find_if(new_set.begin(), new_set.end(), [&](const control_class* control)
    {
        if(this->is_identical_control(control))
        {
            const control_displaycapture* displaycapture_control = 
                (const control_displaycapture*)control;
            this->reference = displaycapture_control;
            component = displaycapture_control->component;

            return true;
        }
        return false;
    });

    if(!component)
    {
        // try to reuse the component stored in the last set's control
        std::find_if(last_set.begin(), last_set.end(), [&](const control_class* control)
        {
            if(this->is_identical_control(control))
            {
                const control_displaycapture* displaycapture_control = 
                    (const control_displaycapture*)control;
                component = displaycapture_control->component;

                return true;
            }
            return false;
        });

        if(!component)
        {
            // create a new component since it was not found in the last or in the new set
            source_displaycapture5_t displaycapture_source(
                new source_displaycapture5(this->pipeline.session, this->pipeline.context_mutex));

            if(this->params.adapter_ordinal == this->pipeline.d3d11dev_adapter)
                displaycapture_source->initialize(
                    this->pipeline.shared_from_this<control_pipeline2>(),
                    this->params.output_ordinal, 
                    this->pipeline.d3d11dev, this->pipeline.devctx);
            else
                displaycapture_source->initialize(
                    this->pipeline.shared_from_this<control_pipeline2>(),
                    this->params.adapter_ordinal, this->params.output_ordinal, 
                    this->pipeline.dxgifactory, this->pipeline.d3d11dev, this->pipeline.devctx);

            component = displaycapture_source;
        }
    }

    new_set.push_back(this);

out:
    this->component = component;
}

void control_displaycapture::list_available_displaycapture_params(
    const control_pipeline2_t& pipeline,
    std::vector<displaycapture_params>& displaycaptures)
{
    assert_(displaycaptures.empty());

    HRESULT hr = S_OK;
    CComPtr<IDXGIAdapter1> adapter;
    for(UINT i = 0; SUCCEEDED(hr = pipeline->dxgifactory->EnumAdapters1(i, &adapter)); i++)
    {
        CComPtr<IDXGIOutput> output;
        for(UINT j = 0; SUCCEEDED(hr = adapter->EnumOutputs(j, &output)); j++)
        {
            displaycapture_params params;
            CHECK_HR(hr = adapter->GetDesc1(&params.adapter));
            CHECK_HR(hr = output->GetDesc(&params.output));
            params.adapter_ordinal = i;
            params.output_ordinal = j;

            displaycaptures.push_back(params);
            output = NULL;
        }

        adapter = NULL;
    }

done:
    if(hr != DXGI_ERROR_NOT_FOUND && FAILED(hr))
        throw HR_EXCEPTION(hr);
}

D2D1_RECT_F control_displaycapture::get_rectangle(bool dest_params) const
{
    const FLOAT width = (FLOAT)std::abs(this->params.output.DesktopCoordinates.right -
        this->params.output.DesktopCoordinates.left),
        height = (FLOAT)std::abs(this->params.output.DesktopCoordinates.bottom -
            this->params.output.DesktopCoordinates.top);

    D2D1_RECT_F rect;
    rect.left = 0.f;
    rect.top = 0.f;
    rect.right = dest_params ? width / 2.f : width;
    rect.bottom = dest_params ? height / 2.f : height;
    return rect;
}

static FLOAT rotation = 0.f;

void control_displaycapture::apply_transformation(
    const D2D1::Matrix3x2F&& transformation, bool dest_params)
{
    const D2D1_RECT_F rect = this->get_rectangle(dest_params);
    stream_videoprocessor2_controller::params_t params;

    this->videoprocessor_params->get_params(params);

    if(dest_params)
    {
        const video_params_t video_params = this->get_video_params(dest_params);
        params.dest_rect = rect;
        params.dest_m = transformation;
        params.axis_aligned_clip = ((video_params.rotate / 90.f) ==
            round(video_params.rotate / 90.f));
    }
    else
    {
        params.source_rect = rect;
        params.source_m = transformation;
    }

    this->videoprocessor_params->set_params(params);
}

void control_displaycapture::set_default_video_params(video_params_t& video_params, bool dest_params)
{
    video_params.rotate = dest_params ? rotation : 0.f;
    video_params.translate = dest_params ? D2D1::Point2F(100.f, 100.f) : D2D1::Point2F();
    video_params.scale = D2D1::Point2F(1.f, 1.f);
}

bool control_displaycapture::is_identical_control(const control_class* control) const
{
    const control_displaycapture* displaycapture_control =
        dynamic_cast<const control_displaycapture*>(control);

    // check that the control is of displaycapture type and it stores a component
    if(!displaycapture_control || !displaycapture_control->component)
        return false;

    // check that component isn't requesting a reinitialization
    if(displaycapture_control->component->get_instance_type() ==
        media_component::INSTANCE_NOT_SHAREABLE)
        return false;

    // check that the control params match this control's params
    return (displaycapture_control->params.adapter_ordinal == this->params.adapter_ordinal &&
        displaycapture_control->params.output_ordinal == this->params.output_ordinal);
}