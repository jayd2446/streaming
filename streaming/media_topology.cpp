#include "media_topology.h"
#include "media_stream.h"
#include <algorithm>

media_topology::media_topology(const presentation_time_source_t& time_source) : 
    clock(new presentation_clock(time_source)), packet_number(0), first_packet_number(0)
{
}

void media_topology::connect_streams(const media_stream_t& stream, const media_stream_t& stream2)
{
    assert_(stream && stream2);

    // make sure that all streams in the reverse topology are unique so that
    // the single request assumption is satisfied
    const bool request_stream = 
        std::find_if(this->topology_reverse.begin(), this->topology_reverse.end(), 
        [&](const topology_t::value_type& val)
    {
        return (std::find(val.second.next.begin(), val.second.next.end(), stream) != 
            val.second.next.end());
    }) != this->topology_reverse.end();

    this->topology[stream.get()].next.push_back(stream2);
    if(!request_stream)
        this->topology_reverse[stream2.get()].next.push_back(stream);
}