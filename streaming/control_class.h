#pragma once
#include "media_stream.h"
#include "media_topology.h"
#include "media_session.h"
#include "enable_shared_from_this.h"
#include "gui_event_handler.h"
#include <string>
#include <memory>
#include <deque>
#include <functional>

class control_pipeline;
class control_scene;
class control_class;

/*

if the component is null(=the control class is deactivated),
the new control must be constructed by finding the control first,
and lastly allocating a new control;

the control needs to expose a function that takes new parameters and
reactivate itself if the component needs to be reinitialized

*/

// void activate walks the parent tree and the root control reactivates the whole tree

// TODO: control class needs explicit referencing;
// explicit referencing simply uses the parameters of the referenced control class

class control_class;
typedef std::shared_ptr<control_class> control_class_t;
typedef std::deque<control_class_t> control_set_t;

class control_class : public enable_shared_from_this
{
    friend class control_scene;
public:
    typedef std::function<void(const control_class_t&)> callable_f;
private:
    control_set_t& active_controls;
protected:
    control_class* parent;

    // for control referencing to work,
    // the topology building calls for controls must happen in the same order
    // as the activation calls

    // a common attribute that every control must implement
    bool disabled;

    // the root control must implement this
    virtual void build_and_switch_topology();
    // source/scene control classes connect to videoprocessor stream only(or not)
    virtual void build_video_topology(const media_stream_t& /*from*/,
        const media_stream_t& /*to*/, const media_topology_t&) 
    {assert_(false);}
    virtual void build_audio_topology(const media_stream_t& /*from*/,
        const media_stream_t& /*to*/, const media_topology_t&) 
    {assert_(false);}
    // activate function can throw;
    // activate function searches the last set for a component and the new set
    // for a control reference;
    // activate must add itself to the new control set if it is succesfully activated;
    // deactivation also breaks a possible circular dependency between the control and its component
    virtual void activate(const control_set_t& last_set, control_set_t& new_set) = 0;

    control_class(control_set_t& active_controls, gui_event_provider& event_provider);
public:
    // name uniquely identifies a control
    std::wstring name;

    // used by control classes to produce events and by gui classes to consume them
    gui_event_provider& event_provider;

    virtual ~control_class() {}

    // pipeline must use this for accessing control classes;
    // NOTE: all locks should be cleared when calling this to avoid possible deadlocking scenarios;
    // returns whether the function was run;
    // no-op if control_pipeline has been deactivated
    virtual bool run_in_gui_thread(callable_f) { assert_(false); return false; }

    // TODO: the encapsulated component must be dismissed if the reactivation needs reinitialization

    // many control attribute changes need the reactivate call;
    // each attribute change that needs a reactivation call should set its encapsulated components
    // to null;

    // (re)activates the whole tree and builds the topology
    void activate();
    void deactivate();
    void disable();

    control_class* get_root();
    bool is_disabled() const { return this->disabled; }
    // returns true if this is found in active set
    bool is_active() const;
};
