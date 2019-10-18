#include "control_scene.h"
#include "control_pipeline.h"
#include "source_empty.h"
#include <algorithm>
#include <iterator>

control_scene::control_scene(control_set_t& active_controls, control_pipeline& pipeline) :
    control_class(active_controls, pipeline.event_provider),
    pipeline(pipeline),
    selected_scene(NULL)
{
}

control_scene::~control_scene()
{
    // remove controls individually so that the events are fired
    while(!this->get_video_controls().empty())
        this->remove_control(true, this->get_video_controls().begin());
    while(!this->get_audio_controls().empty())
        this->remove_control(false, this->get_audio_controls().begin());
}

void control_scene::build_video_topology(const media_stream_t& from,
    const media_stream_t& to, const media_topology_t& topology)
{
    // TODO: scene should include stream videoprocessor controller(or not)

    if(this->disabled)
        return;

    stream_videomixer_base_t videomixer_stream = 
        std::dynamic_pointer_cast<stream_videomixer_base>(to);
    if(!videomixer_stream)
        throw HR_EXCEPTION(E_UNEXPECTED);

    bool no_video = true;
    for(auto&& elem : this->video_controls)
    {
        if(elem->disabled)
            continue;

        no_video = false;
        elem->build_video_topology(from, videomixer_stream, topology);
    }

    if(no_video)
    {
        source_empty_video_t empty_source(new source_empty_video(this->pipeline.session));
        empty_source->initialize(this->pipeline.shared_from_this<control_pipeline>());

        media_stream_t empty_stream = empty_source->create_stream(topology->get_message_generator());
        empty_stream->connect_streams(from, topology);
        videomixer_stream->connect_streams(empty_stream, NULL, topology);
    }
}

void control_scene::build_audio_topology(const media_stream_t& from,
    const media_stream_t& to, const media_topology_t& topology)
{
    if(this->disabled)
        return;

    stream_audiomixer2_base_t audiomixer_stream =
        std::dynamic_pointer_cast<stream_audiomixer2_base>(to);
    if(!audiomixer_stream)
        throw HR_EXCEPTION(E_UNEXPECTED);

    bool no_audio = true;
    // build the subscene audio topology
    for(auto&& elem : this->video_controls)
    {
        control_scene* scene = dynamic_cast<control_scene*>(elem.get());
        if(scene && !scene->disabled)
        {
            no_audio = false;
            scene->build_audio_topology(from, audiomixer_stream, topology);
        }
    }

    for(auto&& elem : this->audio_controls)
    {
        if(elem->disabled)
            continue;

        no_audio = false;
        elem->build_audio_topology(from, audiomixer_stream, topology);
    }

    if(no_audio)
    {
        source_empty_audio_t empty_source(new source_empty_audio(this->pipeline.audio_session));
        empty_source->initialize(this->pipeline.shared_from_this<control_pipeline>());

        media_stream_t empty_stream = empty_source->create_stream(topology->get_message_generator());
        empty_stream->connect_streams(from, topology);
        audiomixer_stream->connect_streams(empty_stream, NULL, topology);
    }
}

void control_scene::activate(const control_set_t& last_set, control_set_t& new_set)
{
    if(this->disabled)
    {
        // deactivate call really cannot be used here because it would deactivate each
        // control individually
        auto f = [&](controls_t& controls)
        {
            for(auto&& elem : controls)
            {
                const bool old_disabled = elem->disabled;
                elem->disabled = true;
                elem->activate(last_set, new_set);
                elem->disabled = old_disabled;
            }
        };
        f(this->video_controls);
        f(this->audio_controls);

        // trigger event
        this->event_provider.for_each([this](gui_event_handler* e) { e->on_scene_activate(this, true); });

        return;
    }

    // add this to the new set;
    // this control must be pushed to the new set before activating new controls
    // so that the ordering stays consistent
    new_set.push_back(this->shared_from_this<control_scene>());

    // activate all video/scene controls
    for(auto&& elem : this->video_controls)
        elem->activate(last_set, new_set);
    // activate all audio controls
    for(auto&& elem : this->audio_controls)
        elem->activate(last_set, new_set);

    // trigger event
    this->event_provider.for_each([this](gui_event_handler* e) { e->on_scene_activate(this, false); });
}

control_displaycapture* control_scene::add_displaycapture(const std::wstring& name, bool add_front)
{
    bool is_video_control, found;
    this->find_control_iterator(name, is_video_control, found);
    if(found)
        return NULL;

    control_displaycapture* ptr;
    control_class_t displaycapture_control(ptr = 
        new control_displaycapture(this->active_controls, this->pipeline));
    displaycapture_control->parent = this;
    displaycapture_control->name = name;
    if(!add_front)
        this->video_controls.push_back(std::move(displaycapture_control));
    else
        this->video_controls.insert(this->video_controls.begin(), std::move(displaycapture_control));

    // trigger event
    if(ptr)
        this->event_provider.for_each([ptr, this](gui_event_handler* e)
            { e->on_control_added(ptr, false, this); });

    return ptr;
}

control_wasapi* control_scene::add_wasapi(const std::wstring& name, bool add_front)
{
    bool is_video_control, found;
    this->find_control_iterator(name, is_video_control, found);
    if(found)
        return NULL;

    control_wasapi* ptr;
    control_class_t wasapi_control(ptr = new control_wasapi(this->active_controls, this->pipeline));
    wasapi_control->parent = this;
    wasapi_control->name = name;
    if(!add_front)
        this->audio_controls.push_back(std::move(wasapi_control));
    else
        this->audio_controls.insert(this->audio_controls.begin(), std::move(wasapi_control));

    // trigger event
    if(ptr)
        this->event_provider.for_each([ptr, this](gui_event_handler* e)
            { e->on_control_added(ptr, false, this); });

    return ptr;
}

control_vidcap* control_scene::add_vidcap(const std::wstring& name, bool add_front)
{
    bool is_video_control, found;
    this->find_control_iterator(name, is_video_control, found);
    if(found)
        return NULL;

    control_vidcap* ptr;
    control_class_t vidcap_control(ptr = new control_vidcap(this->active_controls, this->pipeline));
    vidcap_control->parent = this;
    vidcap_control->name = name;
    if(!add_front)
        this->video_controls.push_back(std::move(vidcap_control));
    else
        this->video_controls.insert(this->video_controls.begin(), std::move(vidcap_control));

    // trigger event
    if(ptr)
        this->event_provider.for_each([ptr, this](gui_event_handler* e)
            { e->on_control_added(ptr, false, this); });

    return ptr;
}

control_scene* control_scene::add_scene(const std::wstring& name, bool add_front)
{
    bool is_video_control, found;
    this->find_control_iterator(name, is_video_control, found);
    if(found)
        return NULL;

    control_scene* ptr;
    control_class_t scene_control(ptr = new control_scene(this->active_controls, this->pipeline));
    scene_control->parent = this;
    scene_control->name = name;
    if(!add_front)
        this->video_controls.push_back(std::move(scene_control));
    else
        this->video_controls.insert(this->video_controls.begin(), std::move(scene_control));

    // trigger event
    if(ptr)
        this->event_provider.for_each([ptr, this](gui_event_handler* e)
            { e->on_control_added(ptr, false, this); });

    return ptr;
}

void control_scene::remove_control(bool is_video_control, controls_t::const_iterator it)
{
    control_class_t control_class = *it;
    control_class->disabled = true;

    is_video_control ? this->video_controls.erase(it) : this->audio_controls.erase(it);

    // TODO: remove the control from the pipeline selection if it exists
    // control_scene currently just deselects all
    this->pipeline.set_selected_control(nullptr, control_pipeline::CLEAR);

    // trigger event
    this->event_provider.for_each([control_class, this](gui_event_handler* e)
        { e->on_control_added(control_class.get(), true, this); });
}

void control_scene::reorder_controls(controls_t::const_iterator src,
    controls_t::const_iterator dst,
    bool is_video_control)
{
    if(src == dst)
        return;

    controls_t& controls = is_video_control ? this->video_controls : this->audio_controls;
    controls.insert(dst, *src);
    controls.erase(src);
}

//control_class* control_scene::find_control(bool is_control_video, int control_index) const
//{
//    if(is_control_video)
//        return (size_t)control_index >= this->video_controls.size() ? NULL : 
//        this->video_controls[control_index].get();
//    else
//        return (size_t)control_index >= this->audio_controls.size() ? NULL :
//        this->audio_controls[control_index].get();
//}

void control_scene::switch_scene(controls_t::const_iterator new_scene)
{
    control_class* new_control = new_scene->get();
    control_class* old_control = this->get_selected_scene();
    assert_(dynamic_cast<control_scene*>(new_control));

    if(new_control == old_control)
        return;

    // deselect old items
    this->pipeline.set_selected_control(nullptr, control_pipeline::CLEAR);

    this->selected_scene = static_cast<control_scene*>(new_control);

    control_class* root = this->get_root();

    // TODO: control_class activate can be only used if the build_and_switch_topology
    // is separated

    control_set_t new_set;
    new_control->disabled = false;
    root->activate(this->active_controls, new_set);

    if(old_control)
    {
        control_set_t new_set2;
        old_control->disabled = true;
        root->activate(new_set, new_set2);

        this->active_controls = std::move(new_set2);
    }
    else
        this->active_controls = std::move(new_set);

    this->build_and_switch_topology();
}

void control_scene::switch_scene(const control_scene& new_scene)
{
    bool is_video_control, found;
    auto it = this->find_control_iterator(new_scene.name, is_video_control, found);
    assert_(is_video_control && found);

    this->switch_scene(it);
}

control_scene* control_scene::get_selected_scene() const
{
    return this->selected_scene;
}

void control_scene::unselect_selected_scene()
{
    this->selected_scene = NULL;
}

control_scene::controls_t::iterator control_scene::find_control_iterator(
    const std::wstring& control_name, bool& is_video_control, bool& found)
{
    found = true;

    is_video_control = true;
    for(auto jt = this->video_controls.begin(); jt != this->video_controls.end(); jt++)
    {
        if((*jt)->name == control_name)
            return jt;
    }

    is_video_control = false;
    for(auto jt = this->audio_controls.begin(); jt != this->audio_controls.end(); jt++)
    {
        if((*jt)->name == control_name)
            return jt;
    }

    found = false;
    return this->audio_controls.end();
}