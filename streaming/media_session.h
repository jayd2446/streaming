#pragma once
#include "media_sample.h"
#include "media_topology.h"
#include <memory>
#include <vector>

// orchestrates the data flow between components in the media pipeline;
// translates the source's media time to the presentation time

/*

on topology change when the original sink sends a request, media session will
send a notifypreroll event to the new sink, and the new sink will request a sample aswell.

mixer transform must wait for all the samples to arrive before mixing. the earliest
time stamp will be the output time stamp.
// (live media sources will generate samples asynchronously based on framerate and update their buffers)

when the media session is ready to deliver the prerolled sample to the new sink, it will
stop the current topology's media sink by stop() and start the new topology with the
old topology's time.(the stop can happen instantly when the media session gets a request)
the new sink either schedules the sample for rendering or drops it.
it also sets a periodic sample request based on that timestamp.

presentation time will by default lag by 1 fps behind the time stamps

(media sink will take a standalone encoding transform as reference.)
media sink itself will also render the frame to live preview window

media sink will periodically request new samples based on the
presentation sample time(e.g 1/60 secs after the first time stamp
new request will be made)

*/

class media_stream;

class media_session
{
private:
    media_topology_t current_topology, new_topology;
public:
    void switch_topology(const media_topology_t& new_topology);

    // event firing functions will return true only if the media session isn't shutdown
    // and the node is added to the media session's topology and the request sample doesn't
    // return fatal_error;
    // the events won't have any effect if those conditions don't apply

    bool request_sample(const media_stream* this_input_stream, bool is_sink) const;
    // is_source flag is used for the media session to able to translate the sample times;
    // sample cannot be NULL
    bool give_sample(
        const media_stream* this_output_stream, 
        const media_sample_t& sample,
        bool is_source) const;
    /*bool give_sample(
        const media_stream* this_output_stream,
        const media_samples_t& samples,
        bool is_source) const;*/

    // changes the format of this_output_stream and changes the input stream of the
    // downstream;
    // returns false if the format negotiation failed
    bool change_format(media_stream* this_output_stream /*, mediatype new_format*/) const;

    // breaks the circular dependency between components and the session
    void shutdown();
};

typedef std::shared_ptr<media_session> media_session_t;