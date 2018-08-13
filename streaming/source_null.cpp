#include "source_null.h"
#include "transform_aac_encoder.h"

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


stream_null::stream_null(const source_null_t& source) : 
    source(source), audio_buffer(new media_buffer_samples)
{
}

media_stream::result_t stream_null::request_sample(request_packet& rp, const media_stream*)
{
    const double frame_duration = SECOND_IN_TIME_UNIT / (double)transform_aac_encoder::sample_rate;
    media_sample_audio audio(media_buffer_samples_t(this->audio_buffer));
    audio.timestamp = rp.request_time;
    audio.bit_depth = sizeof(transform_aac_encoder::bit_depth_t) * 8;
    audio.channels = transform_aac_encoder::channels;
    audio.sample_rate = transform_aac_encoder::sample_rate;
    audio.frame_end = (frame_unit)(rp.request_time / frame_duration);

    return this->process_sample(audio, rp, NULL);
}

media_stream::result_t stream_null::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream*)
{
    return this->source->session->give_sample(this, sample_view, rp, true) ? OK : FATAL_ERROR;
}