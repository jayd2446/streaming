#pragma once
#include "media_component.h"

// TODO: probably unnecessary

class media_sink : public media_component
{
private:
public:
    // stream objects must not be allocated in the constructor
    explicit media_sink(const media_session_t& session) : media_component(session) {}
    virtual ~media_sink() {}
};