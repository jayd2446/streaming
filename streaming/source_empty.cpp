#include "source_empty.h"
#include "transform_aac_encoder.h"
#include "transform_h264_encoder.h"
#include "transform_videomixer.h"
#include "transform_audioprocessor.h"
#include <Mferror.h>

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
    this->callback.Attach(new async_callback_t(&stream_empty_audio::callback_f));
}

void stream_empty_audio::callback_f(void*)
{
    request_packet rp = std::move(this->rp);

    const double frame_duration = SECOND_IN_TIME_UNIT / (double)transform_aac_encoder::sample_rate;
    media_sample_audio audio(media_buffer_samples_t(this->buffer));
    audio.timestamp = rp.request_time;
    audio.bit_depth = sizeof(transform_audioprocessor::bit_depth_t) * 8;
    audio.channels = transform_aac_encoder::channels;
    audio.sample_rate = transform_aac_encoder::sample_rate;
    audio.frame_end = (frame_unit)(rp.request_time / frame_duration);
    audio.silent = true;

    this->process_sample(audio, rp, NULL);
}

media_stream::result_t stream_empty_audio::request_sample(request_packet& rp, const media_stream*)
{
    this->rp = rp;

    const HRESULT hr = this->callback->mf_put_work_item(
        this->shared_from_this<stream_empty_audio>());
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
    else if(hr == MF_E_SHUTDOWN)
        return FATAL_ERROR;

    return OK;
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
    this->callback.Attach(new async_callback_t(&stream_empty_video::callback_f));
}

void stream_empty_video::callback_f(void*)
{
    request_packet rp = std::move(this->rp);

    this->unlock();

    stream_videomixer_controller::params_t params;
    params.dest_rect.left = params.dest_rect.top = 0;
    params.dest_rect.right = transform_h264_encoder::frame_width;
    params.dest_rect.bottom = transform_h264_encoder::frame_height;
    params.source_rect = params.dest_rect;
    params.source_m = params.dest_m = D2D1::Matrix3x2F::Identity();
    params.z_order = 0;

    media_sample_videomixer sample(params);
    sample.timestamp = rp.request_time;
    sample.frame_end = convert_to_frame_unit(sample.timestamp,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);
    sample.frame_start = sample.frame_end - 1;
    sample.silent = true;

    this->source->session->give_sample(this, sample, rp, true);
}

media_stream::result_t stream_empty_video::request_sample(request_packet& rp, const media_stream*)
{
    this->lock();
    this->rp = rp;

    return OK;
}

media_stream::result_t stream_empty_video::process_sample(
    const media_sample&, request_packet&, const media_stream*)
{
    if(this->rp.topology)
    {
        const HRESULT hr = this->callback->mf_put_work_item(
            this->shared_from_this<stream_empty_video>());
        if(FAILED(hr) && hr != MF_E_SHUTDOWN)
            throw HR_EXCEPTION(hr);
        else if(hr == MF_E_SHUTDOWN)
        {
            this->unlock();
            return FATAL_ERROR;
        }
    }

    return OK;
}