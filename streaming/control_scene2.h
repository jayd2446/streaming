#pragma once
#include "control_class.h"
#include "control_displaycapture5.h"
#include "control_wasapi.h"
#include "transform_videoprocessor.h"
#include <string>
#include <vector>

class control_pipeline2;

class control_scene2 : public control_class
{
    friend class control_pipeline2;
public:
    typedef std::vector<control_class_t> controls_t;
private:
    control_pipeline2& pipeline;

    // the last control that was activated using the switch functionality
    bool current_control_video;
    int current_control;

    void build_video_topology_branch(const media_stream_t& to, const media_topology_t&);
    void build_audio_topology_branch(const media_stream_t& to, const media_topology_t&);
    void activate(const control_set_t& last_set, control_set_t& new_set);

    control_class* find_control(bool is_control_video, int control_index) const;

    control_scene2(control_set_t& active_controls, control_pipeline2&);
public:
    // TODO: scene really shouldn't expose these fields but instead have functions
    // video controls include scene controls aswell so that scenes can have ordering
    controls_t video_controls;
    controls_t audio_controls;

    // the controls must be configured before they can be activated,
    // otherwise they will throw;
    // the added controls must be explicitly activated;
    // the name for the control must be unique;
    // returns NULL if the name wasn't unique
    control_displaycapture* add_displaycapture(const std::wstring& name);
    control_wasapi* add_wasapi(const std::wstring& name);
    control_scene2* add_scene(const std::wstring& name);

    void switch_scene(bool is_video_control, int control_index);
    void switch_scene(const control_scene2& new_scene);
    // scene might be null if this scene isn't active or the active scene isn't
    // of scene type
    control_scene2* get_active_scene() const;

    const controls_t& get_video_controls() const {return this->video_controls;}
    const controls_t& get_audio_controls() const {return this->audio_controls;}

    // returns audio_controls.end() if no control was found
    controls_t::iterator find_control_iterator(
        const std::wstring& control_name,
        bool& is_video_control,
        bool& found);

    /*void reactivate();*/
    /*bool is_activated() const;*/
};