#include "stream_worker.h"

stream_worker::stream_worker(const media_component_t& component) : 
    component(component), max_requests(DEFAULT_MAX_REQUESTS), requests(0),
    is_used(true)
{
}

bool stream_worker::is_available() const
{
    return (this->requests < this->max_requests);
}

media_stream::result_t stream_worker::request_sample(request_packet& rp, const media_stream*)
{
    if(this->is_used)
    {
        assert_(this->requests >= 0);

        this->requests++;
        this->component->session->request_sample(this, rp, true);
    }
    else
        this->component->session->request_sample(this, rp, false);

    return OK;
}

media_stream::result_t stream_worker::process_sample(
    const media_sample& sample, request_packet& rp, const media_stream*)
{
    if(this->is_used)
    {
        this->lock();

        this->requests--;
        assert_(this->requests >= 0);

        this->unlock();
    }
    this->component->session->give_sample(this, sample, rp, false);

    return OK;
}