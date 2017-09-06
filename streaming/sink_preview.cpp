#include "sink_preview.h"

sink_preview::sink_preview(const media_session_t& session) : media_sink(session)
{
}

void sink_preview::initialize(HWND)
{
}

media_stream_t sink_preview::create_stream()
{
    media_stream_t temp;
    temp.Attach(new stream_preview(this->shared_from_this()));
    return temp;
}

bool sink_preview::start(media_stream& stream)
{
    return (stream.request_sample() != media_stream::FATAL_ERROR);
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_preview::stream_preview(const sink_preview_t& sink) : sink(sink)
{
}

media_stream::result_t stream_preview::request_sample()
{
    if(!this->sink->session->request_sample(this, true))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_preview::process_sample(const media_sample_t& sample)
{
    media_sample_t sample2 = sample;
    return OK;
}