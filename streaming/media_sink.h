#pragma once
#include "media_session.h"

class media_sink
{
public:
    media_session_t session;

    explicit media_sink(const media_session_t& session) : session(session) {}
    virtual ~media_sink() {}
};