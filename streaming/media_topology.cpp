#include "media_topology.h"
#include "media_stream.h"
#include <algorithm>

media_topology::media_topology(const media_message_generator_t& message_generator) :
    message_generator(message_generator), next_packet_number(0), topology_number(0),
    drained(false)
{
}

void media_topology::connect_streams(const media_stream_t& stream, const media_stream_t& stream2)
{
    assert_(stream && stream2);

    // to make sure that request_sample calls are finished before process_sample calls for a request,
    // source streams are separated from the topology by listing them in an additional container

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
    {
        this->topology_reverse[stream2.get()].next.push_back(stream);

        // source streams are unique in the vector
        if(stream->is_source_stream())
            this->source_streams.push_back(stream);
    }
}