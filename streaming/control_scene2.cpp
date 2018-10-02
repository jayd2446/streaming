#include "control_scene2.h"
#include "control_pipeline2.h"
#include "source_empty.h"
#include <algorithm>
#include <iterator>

#define INVALID_CONTROL_INDEX -1

const std::wstring control_scene2::scene_type_name = L"scene";

control_scene2::control_scene2(control_set_t& active_controls, control_pipeline2& pipeline) :
    control_class(active_controls, scene_type_name, CONTROL_VIDEO_AUDIO_TYPE),
    pipeline(pipeline),
    current_control_video(true),
    current_control(INVALID_CONTROL_INDEX)
{
}

void control_scene2::build_video_topology_branch(
    const media_stream_t& to, const media_topology_t& topology)
{
    // TODO: scene should include stream videoprocessor controller

    if(this->disabled)
        return;

    stream_videoprocessor_t videoprocessor_stream = 
        std::dynamic_pointer_cast<stream_videoprocessor>(to);
    if(!videoprocessor_stream)
        throw HR_EXCEPTION(E_UNEXPECTED);

    if(this->video_controls.empty())
    {
        source_empty_video_t empty_source(new source_empty_video(this->pipeline.session));
        media_stream_t empty_stream = empty_source->create_stream();
        videoprocessor_stream->add_input_stream(empty_stream.get(), NULL);
        videoprocessor_stream->connect_streams(empty_stream, topology);
    }
    else
    {
        for(auto&& elem : this->video_controls)
            elem.second->build_video_topology_branch(videoprocessor_stream, topology);
    }
}

void control_scene2::build_audio_topology_branch(
    const media_stream_t& to, const media_topology_t& topology)
{
    if(this->disabled)
        return;

    // build the subscene audio topology
    bool no_subscene_audio = true;
    if(!this->video_controls.empty())
    {
        for(auto&& elem : this->video_controls)
        {
            control_scene2* scene = dynamic_cast<control_scene2*>(elem.second.get());
            if(scene)
            {
                no_subscene_audio = false;
                scene->build_audio_topology_branch(to, topology);
            }
        }
    }

    if(this->audio_controls.empty() && no_subscene_audio)
    {
        source_empty_audio_t empty_source(new source_empty_audio(this->pipeline.audio_session));
        media_stream_t empty_stream = empty_source->create_stream();
        to->connect_streams(empty_stream, topology);
    }
    else if(!this->audio_controls.empty())
    {
        for(auto&& elem : this->audio_controls)
            elem.second->build_audio_topology_branch(to, topology);
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
                const bool old_disabled = elem.second->disabled;
                elem.second->disabled = true;
                elem.second->activate(last_set, new_set);
                elem.second->disabled = old_disabled;
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
        elem.second->activate(last_set, new_set);
    // activate all audio controls
    for(auto&& elem : this->audio_controls)
        elem.second->activate(last_set, new_set);
}

control_displaycapture& control_scene2::add_displaycapture()
{
    control_displaycapture* ptr;
    control_class_t displaycapture_control(ptr = 
        new control_displaycapture(this->active_controls, this->pipeline));
    displaycapture_control->parent = this;
    this->video_controls.push_back(std::make_pair(control_displaycapture::displaycapture_type_name,
        std::move(displaycapture_control)));
    return *ptr;
}

control_wasapi& control_scene2::add_wasapi()
{
    control_wasapi* ptr;
    control_class_t wasapi_control(ptr = new control_wasapi(this->active_controls, this->pipeline));
    wasapi_control->parent = this;
    this->audio_controls.push_back(std::make_pair(control_wasapi::wasapi_type_name,
        std::move(wasapi_control)));
    return *ptr;
}

control_scene2& control_scene2::add_scene()
{
    control_scene2* ptr;
    control_class_t scene_control(ptr = new control_scene2(this->active_controls, this->pipeline));
    scene_control->parent = this;
    this->video_controls.push_back(std::make_pair(control_scene2::scene_type_name,
        std::move(scene_control)));
    return *ptr;
}

control_class* control_scene2::find_control(bool is_control_video, int control_index) const
{
    if(control_index == INVALID_CONTROL_INDEX)
        return NULL;

    if(is_control_video)
        return this->video_controls[control_index].second.get();
    else
        return this->audio_controls[control_index].second.get();
}

void control_scene2::switch_scene(bool is_video_control, int control_index)
{
    control_class* new_control = this->find_control(is_video_control, control_index);
    control_class* old_control = this->find_control(this->current_control_video, this->current_control);

    if(new_control == old_control)
        return;

    /*

    switch scene:
    for referencing to work:

    new control disabled = false;
    activate(old set, new set);

    old control disabled = true;
    activate(new set, new set 2);

    build and switch topology();

    if the control is disabled or initialization fails,
    it won't be added to the new set

    */

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

    /*this->active_controls = std::move(new_set);*/

    this->build_and_switch_topology();


    //control_set_t add_set = this->active_controls, sub_set;
    //new_control->disabled = false;
    //new_control->activate(this->active_controls, add_set);

    //old_control->disabled = true;
    //old_control->activate(this->active_controls, sub_set);

    //this->current_control_video = is_video_control;
    //this->current_control = control_index;

    //// remove subtracted set from the add set
    //control_set_t diff_set;
    //std::set_difference(add_set.begin(), add_set.end(),
    //    sub_set.begin(), sub_set.end(), std::inserter(diff_set, diff_set.begin()));

    //// set the diff set as the new current set
    //this->pipeline.active_controls = std::move(diff_set);

    //// build the topology
    //this->pipeline.build_and_switch_topology();
}

void control_scene2::switch_scene(const control_scene2& new_scene)
{
    int control_index = 0;
    for(auto&& elem : this->video_controls)
    {
        if(elem.second.get() == &new_scene)
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