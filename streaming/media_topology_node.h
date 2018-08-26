#pragma once
#include "media_session.h"
//#include "media_stream.h"
#include <memory>
#include <atlbase.h> // for ccomptr that is used for media_stream objects

// acts as the baseclass for components that are part of the pipeline;
// also is used to fire events to the media session

class media_topology;

/*

source uses a display capture `device` that has its own loop which updates samples.
the device stays alive as long as there's references to it.
the source simply updates the timestamp on the samples.

sinks won't schedule a new callback if the media session doesn't allow firing events
anymore.

devices won't schedule a new callback if the reference count on them reaches zero.

*/

// TODO: media_topology_node not needed

// enable_shared_from_this allows com reference counting semantics;
// calling shared_from_this without an existing instance of shared_ptr will
// lead to undefined behaviour
class media_topology_node : public std::enable_shared_from_this<media_topology_node>
{
    friend class media_topology;
    friend class media_session;
public:
    typedef std::shared_ptr<media_topology_node> media_topology_node_t;
    typedef std::shared_ptr<media_session> media_session_t;
    typedef std::shared_ptr<media_topology> media_topology_t;
private:
    // ccomptr<media_stream>
public:
    media_session_t session;

    // stream objects must not be allocated in the constructor
    explicit media_topology_node(const media_session_t& session);
    // media_topology_node must remember to manually Release() media_stream instances
    virtual ~media_topology_node() {}

    // this function will allocate the stream objects
    // (or the notifypreroll function)
    virtual void start() = 0;
    // stop() function for finalizing the sink
};

typedef std::shared_ptr<media_topology_node> media_topology_node_t;