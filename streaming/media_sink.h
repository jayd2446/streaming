#pragma once
#include "media_session.h"
#include "media_sample.h"
#include "presentation_clock.h"
#include "enable_shared_from_this.h"

class media_sink : public virtual enable_shared_from_this
{
public:
    media_session_t session;

    media_sink(const media_session_t& session) : session(session) {}
    virtual ~media_sink() {}
};

typedef std::shared_ptr<media_sink> media_sink_t;