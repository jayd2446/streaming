#pragma once
#include "media_stream.h"

template<typename Component>
class stream_worker : public media_stream
{
public:
    typedef Component component_t;
public:
    component_t component;
    volatile bool available;

    explicit stream_worker(const component_t& component);

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};



template<typename T>
stream_worker<T>::stream_worker(const component_t& component) : component(component), available(true)
{
}

template<typename T>
media_stream::result_t stream_worker<T>::request_sample(request_packet& rp, const media_stream*)
{
    if(!this->component->session->request_sample(this, rp, true))
    {
        this->available = true;
        return FATAL_ERROR;
    }

    return OK;
}

template<typename T>
media_stream::result_t stream_worker<T>::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    this->available = true;

    if(!this->component->session->give_sample(this, sample_view, rp, false))
        return FATAL_ERROR;

    return OK;
}