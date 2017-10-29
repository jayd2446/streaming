#pragma once
#include "media_topology.h"
#include "media_sample.h"
#include <mutex>
#include <cassert>

#define INVALID_PACKET_NUMBER -1

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

// the size should be at least equal to the rp size the component expects to receive
// before it processes the rps(aka equal to the size of worker streams);
// FirstPacket is the first packet number the request queue expects
template<int Size, int FirstPacket = 0>
class request_queue
{
public:
    static const int array_size = Size;
    struct request_t
    {
        media_stream* stream;
        const media_stream *prev_stream;
        request_packet rp;
        media_sample_view_t sample_view;
    };
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    std::mutex requests_mutex;
    request_t requests[array_size];
    int pending_packet_number;

    int get_index(int packet_number) const;
    void clear_request(int index);
public:
    request_queue();

    // only the rp field needs to be valid in the request_t
    void push(const request_t&);
    bool pop(request_t&);
};


template<int S, int U>
request_queue<S, U>::request_queue() : pending_packet_number(U)
{
    for(int i = 0; i < array_size; i++)
        this->requests[i].rp.packet_number = INVALID_PACKET_NUMBER;
}

template<int S, int U>
int request_queue<S, U>::get_index(int packet_number) const
{
    return packet_number % array_size;
}

template<int S, int U>
void request_queue<S, U>::clear_request(int index)
{
    this->requests[index].rp.topology = NULL;
    this->requests[index].rp.packet_number = INVALID_PACKET_NUMBER;
    this->requests[index].sample_view = NULL;
}

template<int S, int U>
void request_queue<S, U>::push(const request_t& request)
{
    scoped_lock lock(this->requests_mutex);
    const int index = this->get_index(request.rp.packet_number);
    assert(this->requests[index].rp.packet_number == INVALID_PACKET_NUMBER);

    this->requests[index] = request;
}

template<int S, int U>
bool request_queue<S, U>::pop(request_t& request)
{
    scoped_lock lock(this->requests_mutex);
    const int index = this->get_index(this->pending_packet_number);
    if(this->requests[index].rp.packet_number == INVALID_PACKET_NUMBER)
        return false;

    request = this->requests[index];
    this->clear_request(index);

    this->pending_packet_number++;
    return true;
}