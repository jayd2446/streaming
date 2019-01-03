#include "media_component.h"
#include "control_class.h"
#include "assert.h"

media_component::media_component(const media_session_t& session, instance_t instance_type) :
    session(session), instance_type(instance_type), reset(false)
{
}

void media_component::request_reinitialization(const control_class_t& pipeline)
{
    bool not_reset = false;
    if(this->reset.compare_exchange_strong(not_reset, true))
    {
        // the control_class_t shared ptr typedef should be only used for the root class
        // that won't have a parent class
        assert_(pipeline->get_root() == pipeline.get());

        // set the component as not shareable so that it is recreated when
        // resetting the active scene
        this->instance_type = media_component::INSTANCE_NOT_SHAREABLE;

        // all component locks(those that keep locking)
        // should be unlocked before calling any pipeline functions
        // to prevent possible deadlock scenarios
        control_class::scoped_lock lock(pipeline->mutex);

        // testing is_disabled really won't matter, because
        // the pipeline won't be activated if it is disabled(=shutdown)
        if(!pipeline->is_disabled())
            pipeline->activate();
    }
}