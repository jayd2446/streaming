#pragma once
#include "media_topology.h"
#include "media_sample.h"
#include "assert.h"
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
void request_queue<T>::push(const request_t& request)
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
}

template<class T>
bool request_queue<T>::pop(request_t& request)
{
    scoped_lock lock(this->requests_mutex);
    if(this->get(request))
    {
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
    if(!this->requests.empty())
    {
        if(this->first_packet_number == this->requests.front().rp.packet_number)
        {
            request = this->requests.front();
            return true;
        }
    }

    return false;
}