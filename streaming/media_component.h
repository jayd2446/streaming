#pragma once
#include "media_session.h"
#include "media_sample.h"
#include "presentation_clock.h"
#include "enable_shared_from_this.h"
#include <memory>
#include <mutex>

typedef std::shared_ptr<std::recursive_mutex> context_mutex_t;

class media_component : public virtual enable_shared_from_this
{
public:
    media_session_t session;

    media_component(const media_session_t& session) : session(session) {}
    virtual ~media_component() {}
};

typedef std::shared_ptr<media_component> media_component_t;