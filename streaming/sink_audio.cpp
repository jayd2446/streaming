#include "sink_audio.h"
#include <iostream>

#undef min
#undef max

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

sink_audio::sink_audio(const media_session_t& session) : media_sink(session)
{
}

void sink_audio::initialize()
{
}

stream_audio_t sink_audio::create_stream(media_message_generator_t&& message_generator)
{
    stream_audio_t stream(new stream_audio(this->shared_from_this<sink_audio>()));
    stream->register_listener(message_generator);

    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_audio::stream_audio(const sink_audio_t& sink) : 
    sink(sink), unavailable(0), 
    media_stream_message_listener(sink.get()), 
    stopping(false),
    stop_point(std::numeric_limits<time_unit>::min()),
    requesting(false),
    requests(0), max_requests(DEFAULT_MAX_REQUESTS)
{
}

void stream_audio::on_stream_start(time_unit)
{
    this->requesting = true;
    this->topology = this->sink->session->get_current_topology();
}

void stream_audio::on_stream_stop(time_unit t)
{
    this->stopping = true;
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

        assert_(this->topology);
        this->sink->session->begin_request_sample(this, incomplete_rp, this->topology);
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

media_stream::result_t stream_audio::request_sample(const request_packet& rp, const media_stream*)
{
    if(rp.flags & FLAG_LAST_PACKET)
    {
        assert_(this->stopping);
        this->requesting = false;
    }
    /*if(this->stopping && this->sink->session->is_drainable(this->stop_point, rp.topology))
        this->requesting = false;*/

    if(!this->sink->session->request_sample(this, rp))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_audio::process_sample(
    const media_component_args*, const request_packet&, const media_stream*)
{
    this->requests--;
    return OK;
}