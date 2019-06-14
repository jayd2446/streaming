#pragma once
#include "control_class.h"
#include "control_displaycapture.h"
#include "control_wasapi.h"
#include "control_vidcap.h"
#include "transform_videomixer.h"
#include <string>
#include <vector>
#include <memory>

class control_pipeline;

class control_scene : public control_class
{
    friend class control_pipeline;
    typedef std::unique_ptr<control_class> control_class_t;
public:
    typedef std::vector<control_class_t> controls_t;
private:
    control_pipeline& pipeline;

    // the last control that was activated using the switch functionality
    bool current_control_video;
    int current_control;

    void build_video_topology(const media_stream_t& from,
        const media_stream_t& to, const media_topology_t&);
    void build_audio_topology(const media_stream_t& from,
        const media_stream_t& to, const media_topology_t&);
    void activate(const control_set_t& last_set, control_set_t& new_set);

    control_class* find_control(bool is_control_video, int control_index) const;

    control_scene(control_set_t& active_controls, control_pipeline&);
public:
    // TODO: scene really shouldn't expose these fields but instead have functions
    // video controls include scene controls aswell so that scenes can have ordering;
    // first control appears topmost
    controls_t video_controls;
    controls_t audio_controls;

    // the controls must be configured before they can be activated,
    // otherwise they will throw;
    // the added controls must be explicitly activated;
    // the name for the control must be unique;
    // returns NULL if the name wasn't unique
    control_displaycapture* add_displaycapture(const std::wstring& name, bool add_front = true);
    control_wasapi* add_wasapi(const std::wstring& name, bool add_front = true);
    control_vidcap* add_vidcap(const std::wstring& name, bool add_front = true);
    control_scene* add_scene(const std::wstring& name, bool add_front = false);

    void switch_scene(bool is_video_control, int control_index);
    void switch_scene(const control_scene& new_scene);
    // scene might be null if this scene isn't active or the active scene isn't
    // of scene type
    control_scene* get_active_scene() const;

    const controls_t& get_video_controls() const {return this->video_controls;}
    const controls_t& get_audio_controls() const {return this->audio_controls;}

    // returns audio_controls.end() if no control was found
    controls_t::iterator find_control_iterator(
        const std::wstring& control_name,
        bool& is_video_control,
        bool& found);
};