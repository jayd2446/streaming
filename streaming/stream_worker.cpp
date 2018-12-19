#include "stream_worker.h"

stream_worker::stream_worker(const media_component_t& component) : 
    component(component), max_requests(DEFAULT_MAX_REQUESTS)
{
}

void stream_worker::dispatch_next_request()
{
    // lock is assumed
    request_queue_t::reference request = this->requests.front();
    assert_(!request.topology);
    this->component->session->request_sample(this, request, true);
}

bool stream_worker::is_available() const
{
    scoped_lock lock(this->request_dispatch_mutex);
    return (this->requests.size() < this->max_requests);
}

media_stream::result_t stream_worker::request_sample(request_packet& rp, const media_stream*)
{
    scoped_lock lock(this->request_dispatch_mutex);
    const bool bootstrap_dispatching = this->requests.empty();
    this->requests.push(rp);

    if(bootstrap_dispatching)
        this->dispatch_next_request();

    return OK;
}

media_stream::result_t stream_worker::process_sample(
    const media_sample& sample, request_packet& rp, const media_stream*)
{
    const bool success = this->component->session->give_sample(this, sample, rp, false);
    assert_(success); success;

    scoped_lock lock(this->request_dispatch_mutex);
    this->requests.pop();

    if(!this->requests.empty())
        this->dispatch_next_request();

    return OK;
}