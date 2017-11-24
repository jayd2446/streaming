#pragma once
#include "media_topology.h"
#include "media_sample.h"
#include <mutex>
#include <deque>

#define INVALID_PACKET_NUMBER -1

// used to set source_loopback to discard all samples that have timestamp
// below the request_time
#define AUDIO_DISCARD_PREVIOUS_SAMPLES 1

// request packet has a reference to the topology it belongs to;
// request packets allow for seamless topology switching;
// the old topology stays alive for as long as the request packet
// is alive
struct request_packet
{
    media_topology_t topology;
    int flags;
    time_unit request_time;
    time_unit timestamp;
    // cant be a negative number
    int packet_number;

    bool get_clock(presentation_clock_t&) const;
};

class media_stream;

class request_queue
{
public:
    struct request_t
    {
        media_stream* stream;
        const media_stream* prev_stream;
        request_packet rp;
        media_sample_view_t sample_view;
    };
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    std::recursive_mutex requests_mutex;
    std::deque<request_t> requests;
    int first_packet_number, last_packet_number;

    int get_index(int packet_number) const;
public:
    request_queue();

    // rp needs to be valid
    void push(const request_t&);
    bool pop(request_t&);
    bool get(request_t&);
};