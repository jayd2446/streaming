#pragma once
#include "media_stream.h"
#include "media_component.h"
#include <queue>
#include <mutex>

#define DEFAULT_MAX_REQUESTS 1

// implements buffering by buffering request packets

// TODO: decide if topology switching should be tied to the preview time or the real time;
// for preview time, the request packet should trigger the topology switch

class stream_worker : public media_stream
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef std::queue<request_packet> request_queue_t;
private:
    media_component_t component;
    request_queue_t requests;
    mutable std::mutex request_dispatch_mutex;
    int max_requests;

    void dispatch_next_request();
public:
    explicit stream_worker(const media_component_t& component);

    bool is_available() const;

    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_worker> stream_worker_t;