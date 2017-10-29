#include "media_topology.h"

media_topology::media_topology(const presentation_time_source_t& time_source) : 
    clock(new presentation_clock(time_source)), packet_number(0)
{
}

bool media_topology::connect_streams(const media_stream_t& stream, const media_stream_t& stream2)
{
    this->topology[stream.get()].next.push_back(stream2);
    this->topology_reverse[stream2.get()].next.push_back(stream);

    return true;
}