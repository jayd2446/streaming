#include "control_preview.h"
#include "control_pipeline.h"
#include "wtl.h"

#undef min
#undef max

control_preview::control_preview(control_set_t& active_controls, control_pipeline& pipeline) :
    control_class(active_controls, pipeline.event_provider),
    wnd_preview(pipeline),
    pipeline(pipeline),
    parent(NULL),
    fps(DEFAULT_PREVIEW_FPS)
{
}

void control_preview::build_video_topology(const media_stream_t& from,
    const media_stream_t& to, const media_topology_t& topology)
{
    if(!this->component)
        return;

    assert_(!this->disabled);

    if(!std::dynamic_pointer_cast<stream_videomixer_base>(from))
        throw HR_EXCEPTION(E_UNEXPECTED);

    media_stream_t preview_stream = this->component->create_stream();
    preview_stream->connect_streams(from, topology);
    to->connect_streams(preview_stream, topology);
}

void control_preview::activate(const control_set_t& last_set, control_set_t& new_set)
{
    sink_preview2_t component;

    if(this->disabled)
        goto out;

    // try to reuse the component stored in the last set's control
    {
        auto it = std::find_if(last_set.begin(), last_set.end(), [&](const control_class_t& control)
            {
                if(this->is_identical_control(control))
                {
                    const control_preview* preview_control = (const control_preview*)control.get();
                    component = preview_control->component;

                    return true;
                }
                return false;
            });

        if(it == last_set.end())
        {
            assert_(this->wnd_preview.m_hWnd != NULL);

            sink_preview2_t preview_sink(new sink_preview2(this->pipeline.session));
            preview_sink->initialize(this->pipeline.shared_from_this<control_pipeline>());

            // start the timer
            this->set_state(true);

            component = preview_sink;
        }
    }

    new_set.push_back(this->shared_from_this<control_preview>());

out:
    this->component = component;

    if(this->component)
        this->event_provider.for_each([this](gui_event_handler* e) { e->on_activate(this, false); });
    else
    {
        // stop the timer
        this->set_state(false);
        this->event_provider.for_each([this](gui_event_handler* e) { e->on_activate(this, true); });
    }
}

void control_preview::get_canvas_size(UINT32& width, UINT32& height) const
{
    this->pipeline.videomixer_transform->get_canvas_size(width, height);
}

void control_preview::set_state(bool render)
{
    // halt the timer
    if(!render)
        this->wnd_preview.set_timer(USER_TIMER_MAXIMUM);
    else
        this->set_fps(this->fps);
}

void control_preview::initialize_window(HWND parent)
{
    assert_(this->wnd_preview.m_hWnd == NULL);
    assert_(parent != NULL);

    this->parent = parent;
    this->wnd_preview.Create(this->parent, CWindow::rcDefault, NULL, WS_CHILD);
}

bool control_preview::is_identical_control(const control_class_t& control) const
{
    const control_preview* preview_control = dynamic_cast<const control_preview*>(control.get());

    if(preview_control && preview_control->component && 
        preview_control->component->session != this->pipeline.session)
        return false;

    return (control.get() == this);
}