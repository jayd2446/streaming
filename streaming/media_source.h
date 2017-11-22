#pragma once
#include "media_component.h"

class media_source : public media_component
{
private:
public:
    // stream objects must not be allocated in the constructor
    explicit media_source(const media_session_t& session) : media_component(session) {}
    virtual ~media_source() {}
};