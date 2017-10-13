#pragma once
#include "media_sample.h"
#include "media_topology.h"
#include "presentation_clock.h"
#include <memory>
#include <vector>
#include <map>
#include <mutex>

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

// request packet has a reference to the topology it belongs to;
// request packets allow for seamless topology switching;
// the old topology stays alive for as long as the request packet
// is alive
struct request_packet
{
    media_topology_t topology;
    time_unit request_time;
    time_unit timestamp;
    // cant be a negative number
    int packet_number;
};

class media_session
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    media_topology_t current_topology, new_topology;
    std::recursive_mutex mutex;
public:
    // returns the clock of the current topology;
    // components shouldn't store the reference because it might
    // create a cyclic dependency between clock and components;
    // returns false if the clock is NULL
    bool get_current_clock(presentation_clock_t&) const;
    void switch_topology(const media_topology_t& new_topology);

    // returns false if any of the sinks couldn't be started or stopped
    // TODO: make sure these cannot be called recursively
    bool start_playback(time_unit time_start);
    // returns false if there's no current topology
    bool stop_playback();

    // event firing functions will return true only if the media session isn't shutdown
    // and the node is added to the media session's topology and the request sample doesn't
    // return fatal_error;
    // the events won't have any effect if those conditions don't apply

    // request_sample returns false if the topology is switched
    bool request_sample(
        const media_stream* this_input_stream, 
        request_packet&,
        bool is_sink);
    // is_source flag is used for the media session to able to translate the sample times;
    // sample cannot be NULL
    bool give_sample(
        const media_stream* this_output_stream, 
        const media_sample_view_t& sample_view,
        request_packet&,
        bool is_source);

    // changes the format of this_output_stream and changes the input stream of the
    // downstream;
    // returns false if the format negotiation failed
    bool change_format(media_stream* this_output_stream /*, mediatype new_format*/) const;

    // breaks the circular dependency between components and the session
    void shutdown();
};

typedef std::shared_ptr<media_session> media_session_t;