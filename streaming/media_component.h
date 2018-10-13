#pragma once
#include "media_session.h"
#include "media_sample.h"
#include "presentation_clock.h"
#include "enable_shared_from_this.h"
#include <memory>
#include <mutex>
#include <atomic>

typedef std::shared_ptr<std::recursive_mutex> context_mutex_t;

class control_pipeline2;
typedef std::shared_ptr<control_pipeline2> control_pipeline2_t;

// TODO: media component shouldn't know about control pipeline

// TODO: sink and source component type classes probably useless
class media_component : public virtual enable_shared_from_this
{
public:
    // indicate whether multiple items use the same component instance
    enum instance_t : int
    {
        INSTANCE_SHAREABLE,
        INSTANCE_NOT_SHAREABLE
    };
private:
    std::atomic_bool reset;
protected:
    instance_t instance_type;
    // subsequent calls to this are dismissed;
    // also sets the instance_type as not shareable;
    // make sure that all locks are unlocked before calling this(so no deadlocks occur);
    // multithreading safe
    void request_reinitialization(control_pipeline2_t&);
public:
    media_session_t session;

    media_component(const media_session_t& session, instance_t instance_type = INSTANCE_SHAREABLE);
    virtual ~media_component() {}

    // cache the result because the component might change the type
    // asynchronously
    instance_t get_instance_type() const {return this->instance_type;}
};

typedef std::shared_ptr<media_component> media_component_t;