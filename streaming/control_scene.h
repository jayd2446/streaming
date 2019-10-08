#pragma once
#include "control_class.h"
#include "control_displaycapture.h"
#include "control_wasapi.h"
#include "control_vidcap.h"
#include "transform_videomixer.h"
#include <string>
#include <list>
#include <memory>

/*

control_scene allows 'global' controls that are present in every scene that are contained
in the parent scene

*/

class control_pipeline;

class control_scene final : public control_class
{
    friend class control_pipeline;
public:
    // list is used for invalidation guarantees
    typedef std::list<control_class_t> controls_t;
private:
    control_pipeline& pipeline;
    control_scene* selected_scene;
    // video controls include scene controls aswell so that scenes can have ordering;
    // first control appears topmost
    controls_t video_controls;
    controls_t audio_controls;

    void build_video_topology(const media_stream_t& from,
        const media_stream_t& to, const media_topology_t&);
    void build_audio_topology(const media_stream_t& from,
        const media_stream_t& to, const media_topology_t&);
    void activate(const control_set_t& last_set, control_set_t& new_set);

    void switch_scene(controls_t::const_iterator new_scene);

    control_scene(control_set_t& active_controls, control_pipeline&);
public:
    ~control_scene();

    // the controls must be configured before they can be activated,
    // otherwise they will throw;
    // the added controls must be explicitly activated;
    // the name for the control must be unique;
    // returns NULL if the name wasn't unique
    control_displaycapture* add_displaycapture(const std::wstring& name, bool add_front = true);
    control_wasapi* add_wasapi(const std::wstring& name, bool add_front = true);
    control_vidcap* add_vidcap(const std::wstring& name, bool add_front = true);
    control_scene* add_scene(const std::wstring& name, bool add_front = false);

    // iterator must be valid;
    // sets the control to disabled and erases it from this;
    // also removes the selection from control_pipeline if it exists
    void remove_control(bool is_video_control, controls_t::const_iterator);

    // src control is added in front of the dst control
    void reorder_controls(controls_t::const_iterator src, controls_t::const_iterator dst,
        bool is_video_control = true);

    // new_scene must be contained in this scene
    void switch_scene(const control_scene& new_scene);

    // returns null if there's no selected scene
    control_scene* get_selected_scene() const;
    void unselect_selected_scene();

    const controls_t& get_video_controls() const { return this->video_controls; }
    const controls_t& get_audio_controls() const { return this->audio_controls; }

    // returns audio_controls.end() if no control was found
    controls_t::iterator find_control_iterator(
        const std::wstring& control_name,
        bool& is_video_control,
        bool& found);
};