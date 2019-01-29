#include "source_empty.h"
#include "transform_aac_encoder.h"
#include "transform_h264_encoder.h"
#include "transform_videomixer.h"
#include <Mferror.h>

source_empty_audio::source_empty_audio(const media_session_t& session) : media_component(session)
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
    const media_component_args*, const request_packet& /*rp*/, const media_stream*)
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


source_empty_video::source_empty_video(const media_session_t& session) :
    source_base(session),
    buffer_pool_video_frames(new buffer_pool_video_frames_t)
{
}

source_empty_video::~source_empty_video()
{
    buffer_pool_video_frames_t::scoped_lock lock(this->buffer_pool_video_frames->mutex);
    this->buffer_pool_video_frames->dispose();
}

void source_empty_video::initialize()
{
    this->source_base::initialize(transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);
}

source_empty_video::stream_source_base_t source_empty_video::create_derived_stream()
{
    return stream_empty_video_t(new stream_empty_video(
        this->shared_from_this<source_empty_video>()));
}

bool source_empty_video::get_samples_end(const request_t& request, frame_unit& end)
{
    end = convert_to_frame_unit(request.rp.request_time,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);
    return true;
}

void source_empty_video::make_request(request_t& request, frame_unit frame_end)
{
    media_component_videomixer_args& args = request.sample;
    media_sample_video_mixer_frame frame;

    {
        buffer_pool_video_frames_t::scoped_lock lock(this->buffer_pool_video_frames->mutex);
        args.sample = this->buffer_pool_video_frames->acquire_buffer();
        args.sample->initialize();
    }

    args.frame_end = frame_end;

    frame.pos = this->last_frame_end;
    frame.dur = frame_end - this->last_frame_end;

    args.sample->frames.push_back(frame);
    args.sample->end = frame_end;

    const bool limit_reached =
        args.sample->move_frames_to(NULL, args.sample->end - maximum_buffer_size);
    if(limit_reached)
    {
        std::cout << "source_empty buffer limit reached, excess frames discarded" << std::endl;
    }

    this->last_frame_end = frame_end;
}

void source_empty_video::dispatch(request_t& request)
{
    this->session->give_sample(request.stream, &request.sample, request.rp);
}


///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////


stream_empty_video::stream_empty_video(const source_empty_video_t& source) :
    stream_source_base(source),
    source(source)
{
}

void stream_empty_video::on_component_start(time_unit t)
{
    this->source->last_frame_end = convert_to_frame_unit(t,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);
}