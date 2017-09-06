#pragma once
#include "media_session.h"

class media_source
{
private:
public:
    media_session_t session;

    // stream objects must not be allocated in the constructor
    explicit media_source(const media_session_t& session) : session(session) {}
    virtual ~media_source() {}
};