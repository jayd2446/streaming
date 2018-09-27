#include "media_component.h"
#include "control_pipeline.h"

media_component::media_component(const media_session_t& session, instance_t instance_type) :
    session(session), instance_type(instance_type), reset(false)
{
}

void media_component::request_reinitialization(control_pipeline_t& pipeline)
{
    bool not_reset = false;
    if(this->reset.compare_exchange_strong(not_reset, true))
    {
        // set the component as not shareable so that it is recreated when
        // resetting the active scene
        this->instance_type = media_component::INSTANCE_NOT_SHAREABLE;

        // all component locks(those that keep locking)
        // should be unlocked before calling any pipeline functions
        // to prevent possible deadlock scenarios
        control_pipeline::scoped_lock lock(pipeline->mutex);

        // scene activation isn't possible anymore if the pipeline has been shutdown
        if(pipeline->is_running())
            pipeline->set_active(*pipeline->get_active_scene());
    }
}