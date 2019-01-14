#pragma once
#include "media_stream.h"
#include "media_component.h"

//#define DEFAULT_MAX_REQUESTS 1
// TODO: audio can have more concurrent requests once it is ported from branch level locking
// granularity to stream level
#define VIDEO_MAX_REQUESTS 3

// implements request dropping

// TODO: decide if topology switching should be tied to the preview time or the real time;
// for preview time, the request packet should trigger the topology switch

class stream_worker : public media_stream
{
private:
    media_component_t component;
    int max_requests;
    volatile int requests;
    bool is_used;
public:
    explicit stream_worker(const media_component_t& component);

    bool is_available() const;
    void set_max_requests(int max_requests) {this->max_requests = max_requests;}
    void not_used() {this->is_used = false;}

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_worker> stream_worker_t;