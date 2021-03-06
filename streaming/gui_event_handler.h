#pragma once
#include "assert.h"
#include <vector>
#include <functional>

class control_class;
class control_scene;

struct gui_event_handler
{
    virtual ~gui_event_handler() {}

    // currently it can be assumed that the pipeline is locked while these events
    // are called

    // TODO: start recording etc events

    // NOTE: control_scene triggers on_scene_activate event
    virtual void on_activate(control_class*, bool /*deactivated*/) {}
    virtual void on_scene_activate(control_scene*, bool /*deactivated*/) {}
    // control_scene triggers this;
    // added in this context means adding a control to a scene
    virtual void on_control_added(control_class*, bool /*removed*/, control_scene* /*scene*/) {}
    // control_pipeline triggers this
    virtual void on_control_selection_changed() {}
};

class gui_event_provider final
{
private:
    std::vector<gui_event_handler*> event_handlers;
public:
    ~gui_event_provider();

    // throws if the event handler is already registered
    void register_event_handler(gui_event_handler&);
    // throws if the event handler is not found
    void unregister_event_handler(const gui_event_handler&);

    void for_each(std::function<void(gui_event_handler*)>);
};