#include "control_class.h"
#include "control_pipeline2.h"

control_class::control_class(control_set_t& active_controls) :
    parent(NULL), disabled(false),
    active_controls(active_controls)
{
}

//bool control_class::find_control(
//    const control_set_t& last_set,
//    const control_set_t& new_set,
//    std::function<bool(const control_class*&)> callback)
//{
//    if(std::find_if(last_set.begin(), last_set.end(), callback) != last_set.end())
//        return true;
//    return (std::find_if(new_set.begin(), new_set.end(), callback) != new_set.end());
//}

void control_class::build_and_switch_topology()
{
    this->get_root()->build_and_switch_topology();
}

void control_class::activate()
{
    control_class* root = this->get_root();
    control_set_t new_set;
    root->activate(this->active_controls, new_set);
    this->active_controls = std::move(new_set);

    this->build_and_switch_topology();

    /*assert_(this->parent);
    this->parent->reactivate();*/

    //// this reactivate imitates control_scene2 scene switch

    //assert_(this->parent);
    //this->parent->reactivate();

    //if(this->disabled)
    //    this->active_controls.erase(this);
    //else
    //    this->active_controls.insert(this);
}

control_class* control_class::get_root()
{
    if(this->parent)
        return this->parent->get_root();
    else
        return this;
}

//void control_class::reactivate()
//{
//    /*assert_(this->parent);
//    this->parent->reactivate();*/
//}