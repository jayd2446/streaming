#include "source_empty.h"
#include "transform_aac_encoder.h"
#include "transform_h264_encoder.h"
#include "transform_videomixer.h"
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
    source(source)
{
    this->callback.Attach(new async_callback_t(&stream_empty_audio::callback_f));
}

void stream_empty_audio::callback_f(void*)
{
    request_packet rp = std::move(this->rp);

    this->unlock();

    media_component_audio_args args;
    args.frame_end = convert_to_frame_unit(rp.request_time,
        transform_aac_encoder::sample_rate, 1);

    this->source->session->give_sample(this, &args, rp);
}

media_stream::result_t stream_empty_audio::request_sample(const request_packet& rp, const media_stream*)
{
    this->lock();

    assert_(!this->rp.topology);
    this->rp = rp;

    return OK;
}

media_stream::result_t stream_empty_audio::process_sample(
    const media_component_args*, const request_packet& rp, const media_stream*)
{
    if(this->rp.topology)
    {
        const HRESULT hr = this->callback->mf_put_work_item(
            this->shared_from_this<stream_empty_audio>());
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

    media_component_videomixer_args args(params);
    args.frame_end = convert_to_frame_unit(rp.request_time,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);

    this->source->session->give_sample(this, &args, rp);
}

media_stream::result_t stream_empty_video::request_sample(const request_packet& rp, const media_stream*)
{
    this->lock();

    assert_(!this->rp.topology);
    this->rp = rp;

    return OK;
}

media_stream::result_t stream_empty_video::process_sample(
    const media_component_args*, const request_packet&, const media_stream*)
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