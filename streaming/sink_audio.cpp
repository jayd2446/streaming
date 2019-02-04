#include "sink_audio.h"
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

sink_audio::sink_audio(const media_session_t& session) : media_sink(session)
{
}

void sink_audio::initialize()
{
}

stream_audio_t sink_audio::create_stream(presentation_clock_t&& clock)
{
    stream_audio_t stream(new stream_audio(this->shared_from_this<sink_audio>()));
    stream->register_sink(clock);

    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_audio::stream_audio(const sink_audio_t& sink) : 
    sink(sink), unavailable(0), /*running(false),*/ 
    media_stream_clock_sink(sink.get()), 
    stop_point(std::numeric_limits<time_unit>::min()),
    requesting(false), processing(false),
    requests(0), max_requests(DEFAULT_MAX_REQUESTS)
{
}

void stream_audio::on_stream_start(time_unit)
{
    this->requesting = this->processing = true;
    this->topology = this->sink->session->get_current_topology();
}

void stream_audio::on_stream_stop(time_unit t)
{
    this->requesting = false;
    this->stop_point = t;
}

void stream_audio::dispatch_request(const request_packet& incomplete_rp, bool no_drop)
{
    assert_(this->unavailable <= 240);

    const int requests = this->requests.load();
    if(requests < this->max_requests || no_drop)
    {
        this->requests++;
        this->unavailable = 0;

        this->sink->session->begin_request_sample(this, incomplete_rp);
        assert_(this->topology);
    }
    else
    {
        this->unavailable++;

        std::cout << "--SAMPLE REQUEST DROPPED IN AUDIO_SINK--" << std::endl;
    }

    /*assert_(this->unavailable <= 240);
    assert_(this->running);

    const int j = no_drop ? 0 : 1;

    scoped_lock lock(this->worker_streams_mutex);
    for(auto it = this->worker_streams.begin(); it != (this->worker_streams.end() - j); it++)
    {
        if((*it)->is_available())
        {
            this->unavailable = 0;

            result_t res = (*it)->request_sample(rp, this);
            if(res == FATAL_ERROR)
                std::cout << "couldn't dispatch request on stream audio" << std::endl;

            return;
        }
    }

    assert_(!no_drop);
    std::cout << "--SAMPLE REQUEST DROPPED IN AUDIO_SINK--" << std::endl;
    this->unavailable++;*/
}

void stream_audio::dispatch_process()
{
    this->sink->session->begin_give_sample(this, this->topology);
}

media_stream::result_t stream_audio::request_sample(const request_packet& rp, const media_stream*)
{
    this->requests_queue.initialize_queue(rp);

    if(!this->sink->session->request_sample(this, rp))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_audio::process_sample(
    const media_component_args*, const request_packet& rp, const media_stream*)
{
    this->requests--;

    request_queue::request_t request;
    request.stream = this;
    request.rp = rp;
    this->requests_queue.push(request);

    // check if the last request has been processed and stop further processing in that case
    while(this->requests_queue.pop(request))
	    if(request.rp.request_time == this->stop_point)
	    {
		    assert_(!this->requests_queue.get());
		    this->processing = false;
	    }

    return OK;
}