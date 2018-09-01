#include "source_empty.h"
#include "transform_aac_encoder.h"
#include "transform_h264_encoder.h"
#include "transform_videoprocessor.h"
#include "transform_audioprocessor.h"

// TODO: decide if these empty sources should dispatch requests to work queues
// like normal sources

source_empty_audio::source_empty_audio(const media_session_t& session) : media_source(session)
{
}

media_stream_t source_empty_audio::create_stream()
{
    return media_stream_t(new stream_empty_audio(this->shared_from_this<source_empty_audio>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_empty_audio::stream_empty_audio(const source_empty_audio_t& source) :
    source(source), buffer(new media_buffer_samples)
{
}

media_stream::result_t stream_empty_audio::request_sample(request_packet& rp, const media_stream*)
{
    const double frame_duration = SECOND_IN_TIME_UNIT / (double)transform_aac_encoder::sample_rate;
    media_sample_audio audio(media_buffer_samples_t(this->buffer));
    audio.timestamp = rp.request_time;
    audio.bit_depth = sizeof(transform_audioprocessor::bit_depth_t) * 8;
    audio.channels = transform_aac_encoder::channels;
    audio.sample_rate = transform_aac_encoder::sample_rate;
    audio.frame_end = (frame_unit)(rp.request_time / frame_duration);
    audio.silent = true;

    return this->process_sample(audio, rp, NULL);
}

media_stream::result_t stream_empty_audio::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream*)
{
    return this->source->session->give_sample(this, sample_view, rp, true) ? OK : FATAL_ERROR;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


source_empty_video::source_empty_video(const media_session_t& session) : media_source(session)
{
}

media_stream_t source_empty_video::create_stream()
{
    return media_stream_t(new stream_empty_video(this->shared_from_this<source_empty_video>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_empty_video::stream_empty_video(const source_empty_video_t& source) : 
    source(source),
    buffer(new media_buffer_texture)
{
}

media_stream::result_t stream_empty_video::request_sample(request_packet& rp, const media_stream*)
{
    stream_videoprocessor_controller::params_t params;
    params.enable_alpha = false;
    params.dest_rect.left = params.dest_rect.top = 0;
    params.dest_rect.right = transform_h264_encoder::frame_width;
    params.dest_rect.bottom = transform_h264_encoder::frame_height;
    params.source_rect = params.dest_rect;

    media_sample_videoprocessor sample_view(params, this->buffer);
    sample_view.timestamp = rp.request_time;

    return this->process_sample(sample_view, rp, NULL);
}

media_stream::result_t stream_empty_video::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream*)
{
    return this->source->session->give_sample(this, sample_view, rp, true) ? OK : FATAL_ERROR;
}