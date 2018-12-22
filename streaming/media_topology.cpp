#include "media_topology.h"
#include "media_stream.h"

media_topology::media_topology(const presentation_time_source_t& time_source) : 
    clock(new presentation_clock(time_source)), packet_number(0), first_packet_number(0)
{
}

void media_topology::connect_streams(const media_stream_t& stream, const media_stream_t& stream2)
{
    assert_(stream && stream2);

    int request_streams = 0;
    for(auto&& item : this->topology[stream.get()].next)
        if(item->get_stream_type() == media_stream::PROCESS_REQUEST)
            request_streams++;

    this->topology[stream.get()].next.push_back(stream2);
    if(!request_streams && stream2->get_stream_type() == media_stream::PROCESS_REQUEST)
        this->topology_reverse[stream2.get()].next.push_back(stream);
}