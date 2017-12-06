#include "source_null.h"

source_null::source_null(const media_session_t& session) : media_source(session)
{
}

media_stream_t source_null::create_stream()
{
    return media_stream_t(new stream_null(this->shared_from_this<source_null>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_null::stream_null(const source_null_t& source) : source(source)
{
}

media_stream::result_t stream_null::request_sample(request_packet& rp, const media_stream*)
{
    return this->process_sample(NULL, rp, NULL);
}

media_stream::result_t stream_null::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    return this->source->session->give_sample(this, sample_view, rp, true) ? OK : FATAL_ERROR;
}