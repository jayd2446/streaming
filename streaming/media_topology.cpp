#include "media_topology.h"

media_topology::media_topology() : clock(new presentation_clock)
{
}

bool media_topology::connect_streams(const media_stream_t& stream, const media_stream_t& stream2)
{
    this->topology[stream.get()].next.push_back(stream2);
    this->topology_reverse[stream2.get()].next.push_back(stream);
    //topology_t::mapped_type& node = this->topology[stream];
    return true;
}