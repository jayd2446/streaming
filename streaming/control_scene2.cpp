#include "control_scene2.h"
#include "control_pipeline2.h"
#include "source_empty.h"
#include <algorithm>
#include <iterator>

#define INVALID_CONTROL_INDEX -1

control_scene2::control_scene2(control_set_t& active_controls, control_pipeline2& pipeline) :
    control_class(active_controls, pipeline.mutex),
    pipeline(pipeline),
    current_control_video(true),
    current_control(INVALID_CONTROL_INDEX)
{
}

void control_scene2::build_video_topology(const media_stream_t& from,
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
        empty_source->initialize(this->pipeline.shared_from_this<control_pipeline2>());

        media_stream_t empty_stream = empty_source->create_stream(topology->get_message_generator());
        empty_stream->connect_streams(from, topology);
        videomixer_stream->connect_streams(empty_stream, NULL, topology);
    }
}

void control_scene2::build_audio_topology(const media_stream_t& from,
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
        control_scene2* scene = dynamic_cast<control_scene2*>(elem.get());
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
        empty_source->initialize(this->pipeline.shared_from_this<control_pipeline2>());

        media_stream_t empty_stream = empty_source->create_stream(topology->get_message_generator());
        empty_stream->connect_streams(from, topology);
        audiomixer_stream->connect_streams(empty_stream, NULL, topology);
    }
}

void control_scene2::activate(const control_set_t& last_set, control_set_t& new_set)
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

        return;
    }

    // add this to the new set;
    // this control must be pushed to the new set before activating new controls
    // so that the ordering stays consistent
    new_set.push_back(this);

    // activate all video/scene controls
    for(auto&& elem : this->video_controls)
        elem->activate(last_set, new_set);
    // activate all audio controls
    for(auto&& elem : this->audio_controls)
        elem->activate(last_set, new_set);
}

control_displaycapture* control_scene2::add_displaycapture(const std::wstring& name, bool add_front)
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
    return ptr;
}

control_wasapi* control_scene2::add_wasapi(const std::wstring& name, bool add_front)
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
    return ptr;
}

control_vidcap* control_scene2::add_vidcap(const std::wstring& name, bool add_front)
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

    return ptr;
}

control_scene2* control_scene2::add_scene(const std::wstring& name, bool add_front)
{
    bool is_video_control, found;
    this->find_control_iterator(name, is_video_control, found);
    if(found)
        return NULL;

    control_scene2* ptr;
    control_class_t scene_control(ptr = new control_scene2(this->active_controls, this->pipeline));
    scene_control->parent = this;
    scene_control->name = name;
    if(!add_front)
        this->video_controls.push_back(std::move(scene_control));
    else
        this->video_controls.insert(this->video_controls.begin(), std::move(scene_control));
    return ptr;
}

control_class* control_scene2::find_control(bool is_control_video, int control_index) const
{
    if(control_index == INVALID_CONTROL_INDEX)
        return NULL;

    if(is_control_video)
        return control_index >= this->video_controls.size() ? NULL : 
        this->video_controls[control_index].get();
    else
        return control_index >= this->audio_controls.size() ? NULL :
        this->audio_controls[control_index].get();
}

void control_scene2::switch_scene(bool is_video_control, int control_index)
{
    control_class* new_control = this->find_control(is_video_control, control_index);
    control_class* old_control = this->find_control(this->current_control_video, this->current_control);

    if(new_control == old_control)
        return;

    control_class* root = this->get_root();

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

    this->current_control_video = is_video_control;
    this->current_control = control_index;

    this->build_and_switch_topology();
}

void control_scene2::switch_scene(const control_scene2& new_scene)
{
    int control_index = 0;
    for(auto&& elem : this->video_controls)
    {
        if(elem.get() == &new_scene)
            break;
        control_index++;
    }

    assert_(control_index < this->video_controls.size());

    this->switch_scene(true, control_index);
}

control_scene2* control_scene2::get_active_scene() const
{
    return dynamic_cast<control_scene2*>(this->find_control(
        this->current_control_video, this->current_control));
}

control_scene2::controls_t::iterator control_scene2::find_control_iterator(
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