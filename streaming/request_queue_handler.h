#pragma once
#include "media_component.h"
#include "media_stream.h"
#include "media_sample.h"
#include "request_packet.h"
#include <mutex>

#pragma warning(push)
#pragma warning(disable: 4706) // assignment within conditional expression

// helper base class for serial request serving

template<class Request>
class request_queue_handler
{
public:
    typedef Request request_t;
    typedef request_queue<request_t> request_queue;
private:
    std::mutex serve_mutex, request_queue_mutex;
protected:
    request_queue requests;
    // returns true if the request should be popped from the queue;
    // singlethreaded
    virtual bool on_serve(typename request_queue::request_t&) = 0;
    // returns NULL if cannot serve;
    // singlethreaded
    virtual typename request_queue::request_t* next_request() = 0;
public:
    virtual ~request_queue_handler() {}

    // multithread safe
    void serve();
};


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<typename T>
void request_queue_handler<T>::serve()
{
    std::unique_lock<std::mutex> request_queue_lock(this->request_queue_mutex),
        serve_lock(this->serve_mutex, std::try_to_lock);

    if(!serve_lock)
        return;

    typename request_queue::request_t* request;
    while(request = this->next_request())
    {
        request_queue_lock.unlock();

        if(this->on_serve(*request))
        {
            const bool b = this->requests.pop();
            assert_(b);
        }

        request_queue_lock.lock();
    }
}

#pragma warning(pop)