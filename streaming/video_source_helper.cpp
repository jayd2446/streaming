#include "video_source_helper.h"
#include <iostream>

video_source_helper::video_source_helper() :
    initialized(false),
    buffer_pool_video_frames(new buffer_pool_video_frames_t),
    fully_initialized(false)
{
}

video_source_helper::~video_source_helper()
{
    buffer_pool_video_frames_t::scoped_lock lock(this->buffer_pool_video_frames->mutex);
    this->buffer_pool_video_frames->dispose();
}

void video_source_helper::add_padding_frames(
    const media_sample_video_mixer_frame& last_frame,
    frame_unit next_frame_pos,
    const media_sample_video_mixer_frames_t& sample) const
{
    const frame_unit last_end = last_frame.pos + last_frame.dur;
    const frame_unit skipped_frames_dur = next_frame_pos - last_end;
    if(skipped_frames_dur > 0)
    {
        media_sample_video_mixer_frame duplicate_frame;
        duplicate_frame.pos = last_end;
        duplicate_frame.dur = skipped_frames_dur;
        duplicate_frame.buffer = last_frame.buffer;
        duplicate_frame.params = last_frame.params;

        sample->add_consecutive_frames(duplicate_frame);
    }
}

void video_source_helper::initialize(frame_unit start,
    frame_unit frame_rate_num, frame_unit frame_rate_den,
    bool serve_null_samples_before_first_sample)
{
    media_sample_video_mixer_frame first_frame;
    first_frame.pos = start - 1;
    first_frame.dur = 1;
    this->last_served_frame = first_frame;
    this->framerate = {frame_rate_num, frame_rate_den};

    this->initialized = true;

    if(!serve_null_samples_before_first_sample)
        this->fully_initialized = true;
}

bool video_source_helper::get_samples_end(time_unit request_time, frame_unit& end) const
{
    if(!this->fully_initialized)
    {
        end = convert_to_frame_unit(request_time,
            this->framerate.first,
            this->framerate.second);
        return true;
    }
    else if(this->captured_frames.empty())
        return false;
    else
    {
        end = this->captured_frames.back().end();
        return true;
    }
}

void video_source_helper::add_new_sample(const media_sample_video_mixer_frame& new_sample)
{
    assert_(this->captured_frames.size() <= maximum_frame_count);
    assert_(new_sample.dur == 1);

    this->fully_initialized = true;

    if(this->captured_frames.size() == maximum_frame_count)
    {
        std::cout << "captured frame dropped" << std::endl;
        this->captured_frames.pop();
    }

    this->captured_frames.push(new_sample);
}

media_sample_video_mixer_frames_t video_source_helper::make_sample(frame_unit frame_end)
{
    assert_(this->initialized);

    // TODO: video source helper should be made more robust so that it could
    // return empty collections aswell

    media_sample_video_mixer_frames_t sample;
    {
        buffer_pool_video_frames_t::scoped_lock lock(this->buffer_pool_video_frames->mutex);
        sample = this->buffer_pool_video_frames->acquire_buffer();
    }
    sample->initialize();

    // add captured frames to the collection and insert padding frames
    while(!this->captured_frames.empty())
    {
        if(this->captured_frames.front().pos < frame_end)
        {
            this->add_padding_frames(this->last_served_frame, this->captured_frames.front().pos,
                sample);
            sample->add_consecutive_frames(this->captured_frames.front());

            this->last_served_frame = this->captured_frames.front();
            this->captured_frames.pop();
        }
        else
        {
            // add the last served frame if no captured frames were added
            /*if(sample->frames.empty())
                sample->add_consecutive_frames(this->last_served_frame);*/

            break;
        }
    }

    // TODO: add the last served frame here(probably) and remove from above
    if(!sample->is_valid())
        sample->add_consecutive_frames(this->last_served_frame);

    // add padding up to frame_end
    this->add_padding_frames(this->last_served_frame, frame_end, sample);

    assert_(sample->get_end() == frame_end);
    this->last_served_frame.pos = frame_end - 1;
    this->last_served_frame.dur = 1;

    // keep the frames buffer within the limits
    if(sample->move_frames_to(NULL, sample->get_end() - maximum_buffer_size))
        std::cout << "frame limit reached; frames skipped" << std::endl;

    assert_(sample->is_valid());
    return sample;
}