#include "media_topology.h"

bool media_topology::connect_streams(const media_stream_t& stream, const media_stream_t& stream2)
{
    this->topology[stream].next.push_back(stream2);
    this->topology_reverse[stream2].next.push_back(stream);
    //topology_t::mapped_type& node = this->topology[stream];
    return true;
}