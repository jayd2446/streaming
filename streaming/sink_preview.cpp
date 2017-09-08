#include "sink_preview.h"
#include <iostream>

sink_preview::sink_preview(const media_session_t& session) : media_sink(session)
{
}

void sink_preview::initialize(HWND)
{
}

media_stream_t sink_preview::create_stream(presentation_clock_t& clock)
{
    media_stream_t temp;
    temp.Attach(new stream_preview(
        std::dynamic_pointer_cast<sink_preview>(this->shared_from_this()), clock));
    return temp;
}

//bool sink_preview::start(media_stream& stream)
//{
//    return (stream.request_sample() != media_stream::FATAL_ERROR);
//}

//bool sink_preview::on_clock_start(time_unit t)
//{
//    return (stream.request_sample() != media_stream::FATAL_ERROR);
//}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_preview::stream_preview(const sink_preview_t& sink, presentation_clock_t& clock) : 
    sink(sink), presentation_clock_sink(clock)
{
}

bool stream_preview::on_clock_start(time_unit t)
{
    return (this->request_sample() != media_stream::FATAL_ERROR);
}

void stream_preview::on_clock_stop(time_unit t)
{
    this->clear_queue();
}

void stream_preview::scheduled_callback(time_unit due_time)
{
    this->request_sample();

    std::cout << this->get_clock()->get_current_time() << std::endl;

    // TODO: swap buffers
}

presentation_clock_t stream_preview::get_clock()
{
    return this->sink->session->get_current_clock();
}

media_stream::result_t stream_preview::request_sample()
{
    if(!this->sink->session->request_sample(this, true))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_preview::process_sample(const media_sample_t& sample)
{
    // schedule the sample
    // 5000 = half a second
    this->schedule_callback(sample->timestamp + 5000000 * 2);

    // TODO: render the sample to the backbuffer here

    return OK;
}