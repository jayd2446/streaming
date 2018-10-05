#pragma once
#include "media_stream.h"
#include "media_topology.h"
#include "media_session.h"
#include <string>
#include <memory>
#include <deque>
//#include <functional>

class control_pipeline2;
class control_scene2;
class control_class;
typedef std::unique_ptr<control_class> control_class_t;

//enum control_common_type : int
//{
//    CONTROL_VIDEO_TYPE = 0x1,
//    CONTROL_AUDIO_TYPE = 0x2,
//    CONTROL_VIDEO_AUDIO_TYPE = CONTROL_VIDEO_TYPE | CONTROL_AUDIO_TYPE,
//};

/*

if the component is null(=the control class is deactivated),
the new control must be constructed by finding the control first,
and lastly allocating a new control;

the control needs to expose a function that takes new parameters and
reactivate itself if the component needs to be reinitialized

*/

// control_class(audio session, video session)
// control_class* parent;

// virtual void activate(last control set, new control set, audio session, video session)
// void activate walks the parent tree and the root control reactivates the whole tree

// TODO: control class needs explicit referencing;
// explicit referencing simply uses the parameters of the referenced control class

typedef std::deque<control_class*> control_set_t;

class control_class
{
    friend class control_scene2;
protected:
    control_class* parent;
    control_set_t& active_controls;

    // for control referencing to work,
    // the topology building calls for controls must happen in the same order
    // as the activation calls

    // a common attribute that every control must implement
    bool disabled;

    // the root control must implement this
    virtual void build_and_switch_topology();
    // source/scene control classes connect to videoprocessor stream only
    virtual void build_video_topology_branch(const media_stream_t& /*to*/, const media_topology_t&) 
    {assert_(false);}
    virtual void build_audio_topology_branch(const media_stream_t& /*to*/, const media_topology_t&) 
    {assert_(false);}
    // activate function can throw;
    // activate function searches the last set for a component and the new set
    // for a control reference;
    // activate must add itself to the new control set if it is succesfully activated
    virtual void activate(const control_set_t& last_set, control_set_t& new_set) = 0;

    explicit control_class(control_set_t& active_controls);
public:
    // name uniquely identifies a control
    std::wstring name;
    /*const std::wstring type_name;
    const control_common_type type;*/

    virtual ~control_class() {}

    // TODO: the encapsulated component must be dismissed if the reactivation needs reinitialization

    // many control attribute changes need the reactivate call;
    // each attribute change that needs a reactivation call should set its encapsulated components
    // to null;

    // (re)activates the whole tree and builds the topology
    void activate();
    void deactivate() {this->disabled = true; this->activate(); this->disabled = false;}
    void disable() {this->disabled = true; this->activate();}

    control_class* get_root();

    // is activated checks whether the encapsulated component is non-null
    /*virtual bool is_activated() const = 0;*/
    bool is_disabled() const {return this->disabled;}
};