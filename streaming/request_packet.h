#pragma once
#include "media_topology.h"
#include "media_sample.h"
#include "assert.h"
#include <mutex>
#include <deque>

#define INVALID_PACKET_NUMBER -1

// indicates a gap in the stream
#define FLAG_DISCONTINUITY 0x1

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

// TODO: change the name in request_t from sample_view to sample
template<class SampleView>
class request_queue
{
public:
    typedef SampleView sample_view_t;
    struct request_t
    {
        media_stream* stream;
        const media_stream* prev_stream;
        request_packet rp;
        sample_view_t sample_view;

        request_t() {}
        // TODO: decide if just remove the explicit defaults
        request_t(const request_t&) = default;
        request_t& operator=(const request_t&) = default;
        request_t(request_t&&) = default;
        request_t& operator=(request_t&&) = default;
    };
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    mutable std::mutex requests_mutex;
    std::deque<request_t> requests;
    int first_packet_number, last_packet_number;

    int get_index(int packet_number) const;
    bool can_pop() const;
public:
    request_queue();

    // rp needs to be valid
    request_t& push(const request_t&);
    // pop will use move semantics
    bool pop(request_t&);
    // NOTE: get is slow for non trivial sample types
    bool get(request_t&);
};



template<class T>
request_queue<T>::request_queue() : 
    first_packet_number(INVALID_PACKET_NUMBER), last_packet_number(INVALID_PACKET_NUMBER)
{
}

template<class T>
int request_queue<T>::get_index(int packet_number) const
{
    return packet_number - this->first_packet_number;
}

template<class T>
bool request_queue<T>::can_pop() const
{
    // lock is assumed
    if(!this->requests.empty())
        if(this->first_packet_number == this->requests.front().rp.packet_number)
            return true;

    return false;
}

template<class T>
typename request_queue<T>::request_t& request_queue<T>::push(const request_t& request)
{
    scoped_lock lock(this->requests_mutex);
    // starting packet number must be initialized lazily
    if(this->first_packet_number == INVALID_PACKET_NUMBER)
    {
        assert_(this->requests.empty());
        this->first_packet_number = 
            this->last_packet_number = request.rp.topology->get_first_packet_number();
        this->requests.push_back(request);
    }

    // queue won't work properly if the first packet number is greater than
    // the one in the submitted request
    assert_(request.rp.packet_number >= this->first_packet_number);
    if(request.rp.packet_number > this->last_packet_number)
    {
        const int diff = request.rp.packet_number - this->last_packet_number;
        this->requests.insert(this->requests.end(), diff, request);
        this->last_packet_number = request.rp.packet_number;
    }
    else
        this->requests[this->get_index(request.rp.packet_number)] = request;

    // references to deque elements will stay valid
    return this->requests[this->get_index(request.rp.packet_number)];
}

template<class T>
bool request_queue<T>::pop(request_t& request)
{
    scoped_lock lock(this->requests_mutex);
    if(this->can_pop())
    {
        request = std::move(this->requests.front());
        this->requests.pop_front();
        this->first_packet_number++;
        return true;
    }

    return false;
}

template<class T>
bool request_queue<T>::get(request_t& request)
{
    scoped_lock lock(this->requests_mutex);
    if(this->can_pop())
    {
        request = this->requests.front();
        return true;
    }

    return false;
}