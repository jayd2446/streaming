#pragma once
#include "media_component.h"

// TODO: media source should be divided to media_source_video and audio
// which both inherit from media_component;
// this is only if duplication will be a property of every media component object
// (actually, connecting a stream to multiple inputs should do that already)

// videoprocessor treats stream defined parameters as absolute
// and user defined parameters as relative

// TODO: transforms should inherit directly from media component

class media_source : public media_component
{
private:
public:
    // stream objects must not be allocated in the constructor
    explicit media_source(const media_session_t& session) : media_component(session) {}
    virtual ~media_source() {}
};