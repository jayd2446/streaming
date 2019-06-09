#pragma once
#include "media_topology.h"
#include "media_sample.h"
#include "assert.h"
#include <mutex>
#include <deque>
#include <atomic>

#define INVALID_PACKET_NUMBER -1

// indicates a gap in the stream
#define FLAG_DISCONTINUITY 0x1
// indicates a last request packet in the topology
#define FLAG_LAST_PACKET 0x2

// TODO: request_packet should probably have the media_ prefix

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
};

class media_stream;

template<class Sample>
class request_queue
{
public:
    typedef Sample sample_t;
    struct request_t
    {
        media_stream* stream;
        // prev_stream really cannot be used because it excludes multi-input streams
        /*const media_stream* prev_stream;*/
        request_packet rp;
        // TODO: rename to args and args_t(or payload)
        sample_t sample;

        request_t() = default;
        // TODO: decide if just remove the explicit defaults;
        // if there are no user defined special constructors apart from the 'constructor',
        // the move constructor should be implicitly declared;
        // same goes for the move assignment

        // implicit copy constructor and assignment are deleted if move semantics are
        // explicitly declared
        request_t(const request_t&) = default;
        request_t& operator=(const request_t&) = default;
        request_t(request_t&&) = default;
        request_t& operator=(request_t&&) = default;
    };
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
private:
    struct single_request_queue
    {
        std::deque<request_t> requests;
        int first_packet_number, last_packet_number;
    };
private:
    mutable std::recursive_mutex requests_mutex;
    std::deque<single_request_queue> requests;
    int first_topology_number, last_topology_number;
    std::atomic_bool initialized;

    int get_topology_index(int topology_number) const;
    int get_index(int topology_index, int packet_number) const;
    bool can_pop() const;
    // moves to next queue;
    // current queue must be empty before moving
    void next_topology();
public:
    request_queue();

    // initialize_queue must be called in the request_sample function;
    // failure to do so causes a race condition with the first packet number
    // initialization
    void initialize_queue(const request_packet&);

    // rp needs to be valid;
    // if a request with the same packet number already exists, the old request is replaced;
    // NOTE: push is slow for non trivial arg types
    void push(const request_t&);
    // pop will use move semantics;
    // pop will advance the queue if the popped request was tagged with flag_last_packet
    bool pop(request_t&);
    bool pop();
    // NOTE: get is slow for non trivial sample types
    bool get(request_t&) const;

    // NOTE: the returned item is only valid until it is popped, which can lead to
    // undefined behaviour without explicit locking
    // returns NULL if couldn't get
    request_t* get();
    request_t* get(int packet_number);
};



template<class T>
request_queue<T>::request_queue() : 
    first_topology_number(-1), last_topology_number(-1),
    initialized(false)
{
}

template<class T>
int request_queue<T>::get_topology_index(int topology_number) const
{
    return topology_number - this->first_topology_number;
}

template<class T>
int request_queue<T>::get_index(int topology_index, int packet_number) const
{
    return packet_number - this->requests[topology_index].first_packet_number;
}

template<class T>
bool request_queue<T>::can_pop() const
{
    // lock is assumed
    if(!this->requests.empty() &&
        !this->requests[this->get_topology_index(this->first_topology_number)].requests.empty())
    {
        const single_request_queue& queue =
            this->requests[this->get_topology_index(this->first_topology_number)];
        if(queue.first_packet_number == queue.requests.front().rp.packet_number)
            return true;
    }

    return false;
}

template<class T>
void request_queue<T>::next_topology()
{
    // lock is assumed

    // there must be a valid topology
    assert_(!this->requests.empty());

    single_request_queue& queue =
        this->requests[this->get_topology_index(this->first_topology_number)];

    // the current topology queue must be empty
    assert_(queue.requests.empty());

    this->requests.pop_front();
    this->first_topology_number++;
}

template<class T>
void request_queue<T>::initialize_queue(const request_packet& rp)
{
    assert_(rp.topology);

    bool not_initialized = false;
    if(this->initialized.compare_exchange_strong(not_initialized, true))
    {
        scoped_lock lock(this->requests_mutex);

        // add the first request to the first queue
        single_request_queue queue;
        queue.first_packet_number = queue.last_packet_number = 0;

        request_t placeholder_request;
        placeholder_request.rp.packet_number = INVALID_PACKET_NUMBER;
        queue.requests.push_back(std::move(placeholder_request));

        // add the first queue to this
        this->first_topology_number =
            this->last_topology_number = rp.topology->get_topology_number();

        this->requests.push_back(std::move(queue));
    }
}

template<class T>
void request_queue<T>::push(const request_t& request)
{
    scoped_lock lock(this->requests_mutex);

    // the queue must have been initialized in the request sample function
    assert_(this->first_topology_number != -1);

    const int diff = request.rp.topology->get_topology_number() - this->last_topology_number;
    // queue won't work properly if the first topology number is greater than
    // the one in the submitted request
    assert_(request.rp.topology->get_topology_number() >= this->first_topology_number);

    if(diff > 0)
    {
        this->last_topology_number = request.rp.topology->get_topology_number();

        // add new queue(s)
        single_request_queue queue;
        queue.first_packet_number = queue.last_packet_number = 0;

        request_t placeholder_request;
        placeholder_request.rp.packet_number = INVALID_PACKET_NUMBER;
        queue.requests.push_back(std::move(placeholder_request));

        this->requests.insert(this->requests.end(), diff, queue);

        // add the new request
        this->push(request);
    }
    else
    {
        const int topology_index = this->get_topology_index(request.rp.topology->get_topology_number());
        single_request_queue& queue = this->requests[topology_index];

        // the queue must be valid
        assert_(queue.first_packet_number != INVALID_PACKET_NUMBER);

        const int diff2 = request.rp.packet_number - queue.last_packet_number;
        // queue won't work properly if the first packet number is greater than
        // the one in the submitted request
        assert_(request.rp.packet_number >= queue.first_packet_number);

        if(diff2 > 0)
        {
            queue.last_packet_number = request.rp.packet_number;
            queue.requests.insert(queue.requests.end(), diff2, request);
        }
        else
            queue.requests[this->get_index(topology_index, request.rp.packet_number)] = request;
    }
}

template<class T>
bool request_queue<T>::pop(request_t& request)
{
    scoped_lock lock(this->requests_mutex);
    if(this->can_pop())
    {
        single_request_queue& queue =
            this->requests[this->get_topology_index(this->first_topology_number)];

        const bool next = (queue.requests.front().rp.flags & FLAG_LAST_PACKET);

        request = std::move(queue.requests.front());
        queue.requests.pop_front();
        queue.first_packet_number++;

        if(next)
            this->next_topology();

        return true;
    }

    return false;
}

template<class T>
bool request_queue<T>::pop()
{
    scoped_lock lock(this->requests_mutex);
    if(this->can_pop())
    {
        single_request_queue& queue =
            this->requests[this->get_topology_index(this->first_topology_number)];

        const bool next = (queue.requests.front().rp.flags & FLAG_LAST_PACKET);

        queue.requests.pop_front();
        queue.first_packet_number++;

        if(next)
            this->next_topology();

        return true;
    }

    return false;
}

template<class T>
bool request_queue<T>::get(request_t& request) const
{
    scoped_lock lock(this->requests_mutex);
    if(this->can_pop())
    {
        single_request_queue& queue =
            this->requests[this->get_topology_index(this->first_topology_number)];

        request = queue.requests.front();

        return true;
    }

    return false;
}

template<class T>
typename request_queue<T>::request_t* request_queue<T>::get()
{
    scoped_lock lock(this->requests_mutex);
    if(this->can_pop())
    {
        single_request_queue& queue =
            this->requests[this->get_topology_index(this->first_topology_number)];

        // references to deque elements will stay valid
        return &queue.requests[0];
    }

    return NULL;
}

template<class T>
typename request_queue<T>::request_t* request_queue<T>::get(int packet_number)
{
    scoped_lock lock(this->requests_mutex);

    const int topology_index = this->get_topology_index(this->first_topology_number);
    assert_(topology_index >= 0);

    if(topology_index < this->requests.size())
    {
        single_request_queue& queue = this->requests[topology_index];

        const int index = this->get_index(topology_index, packet_number);
        assert_(index >= 0);

        if(index < queue.requests.size())
            return &queue.requests[index];
    }

    return NULL;
}