#include "source_empty.h"
#include <Mferror.h>

source_empty_audio::source_empty_audio(const media_session_t& session) :
    source_base(session),
    buffer_pool_audio_frames(new buffer_pool_audio_frames_t)
{
}

source_empty_audio::~source_empty_audio()
{
    buffer_pool_audio_frames_t::scoped_lock lock(this->buffer_pool_audio_frames->mutex);
    this->buffer_pool_audio_frames->dispose();
}

void source_empty_audio::initialize(const control_class_t& ctrl_pipeline)
{
    this->source_base::initialize(ctrl_pipeline);
}

source_empty_audio::stream_source_base_t source_empty_audio::create_derived_stream()
{
    return stream_empty_audio_t(new stream_empty_audio(this->shared_from_this<source_empty_audio>()));
}

bool source_empty_audio::get_samples_end(time_unit request_time, frame_unit& end) const
{
    end = convert_to_frame_unit(request_time, 
        this->session->frame_rate_num, this->session->frame_rate_den);
    return true;
}

void source_empty_audio::make_request(request_t& request, frame_unit frame_end)
{
    if(this->last_frame_end == frame_end)
        return;

    request.sample.args = std::make_optional<media_component_audiomixer_args>();
    media_component_audiomixer_args& args = *request.sample.args;
    media_sample_audio_mixer_frame frame;

    {
        buffer_pool_audio_frames_t::scoped_lock lock(this->buffer_pool_audio_frames->mutex);
        args.sample = this->buffer_pool_audio_frames->acquire_buffer();
        args.sample->initialize();
    }

    args.frame_end = frame_end;

    frame.pos = this->last_frame_end;
    frame.dur = frame_end - this->last_frame_end;
    args.sample->add_consecutive_frames(frame);

    const bool limit_reached =
        args.sample->move_frames_to(NULL, args.sample->end - this->get_maximum_buffer_size(),
            transform_audiomixer2::block_align);
    if(limit_reached)
    {
        std::cout << "source_empty_audio buffer limit reached, excess frames discarded" << std::endl;
    }

    this->last_frame_end = frame_end;
}

void source_empty_audio::dispatch(request_t& request)
{
    this->session->give_sample(request.stream, request.sample.args.has_value() ?
        &(*request.sample.args) : NULL, request.rp);
}


///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////


stream_empty_audio::stream_empty_audio(const source_empty_audio_t& source) :
    stream_source_base(source),
    source(source)
{
}

void stream_empty_audio::on_component_start(time_unit t)
{
    this->source->last_frame_end = convert_to_frame_unit(t,
        this->source->session->frame_rate_num, this->source->session->frame_rate_den);
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

void source_empty_video::initialize(const control_class_t& ctrl_pipeline)
{
    this->source_base::initialize(ctrl_pipeline);
}

source_empty_video::stream_source_base_t source_empty_video::create_derived_stream()
{
    return stream_empty_video_t(new stream_empty_video(this->shared_from_this<source_empty_video>()));
}

bool source_empty_video::get_samples_end(time_unit request_time, frame_unit& end) const
{
    end = convert_to_frame_unit(request_time,
        this->session->frame_rate_num, this->session->frame_rate_den);
    return true;
}

void source_empty_video::make_request(request_t& request, frame_unit frame_end)
{
    if(this->last_frame_end == frame_end)
        return;

    request.sample.args = std::make_optional<media_component_videomixer_args>();
    media_component_videomixer_args& args = *request.sample.args;
    media_sample_video_mixer_frame frame;

    {
        buffer_pool_video_frames_t::scoped_lock lock(this->buffer_pool_video_frames->mutex);
        args.sample = this->buffer_pool_video_frames->acquire_buffer();
        args.sample->initialize();
    }

    args.frame_end = frame_end;

    frame.pos = this->last_frame_end;
    frame.dur = frame_end - this->last_frame_end;
    args.sample->add_consecutive_frames(frame);

    const bool limit_reached =
        args.sample->move_frames_to(NULL, args.sample->end - maximum_buffer_size);
    if(limit_reached)
    {
        std::cout << "source_empty_video buffer limit reached, excess frames discarded" << std::endl;
    }

    this->last_frame_end = frame_end;
}

void source_empty_video::dispatch(request_t& request)
{
    this->session->give_sample(request.stream, request.sample.args.has_value() ?
        &(*request.sample.args) : NULL, request.rp);
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
        this->source->session->frame_rate_num, this->source->session->frame_rate_den);
}