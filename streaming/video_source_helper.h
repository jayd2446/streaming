#pragma once
#include "source_base.h"
#include "transform_h264_encoder.h"
#include "transform_videomixer.h"
#include <queue>

// provides common functionality for video sources, such as keeping sample buffering within
// the limits and duplicating skipped frames

// TODO: add timeout so that a stalling source doesn't stall the pipeline

class video_source_helper
{
public:
    static const size_t maximum_frame_count = 10;
    static const frame_unit maximum_buffer_size = 60;
    typedef buffer_pool<media_sample_video_mixer_frames_pooled> buffer_pool_video_frames_t;
private:
    std::queue<media_sample_video_mixer_frame> captured_frames;
    std::shared_ptr<buffer_pool_video_frames_t> buffer_pool_video_frames;
    media_sample_video_mixer_frame last_served_frame;

    bool fully_initialized, broken;

    void add_padding_frames(const media_sample_video_mixer_frame& last_frame,
        frame_unit next_frame_pos,
        const media_sample_video_mixer_frames_t&) const;
public:
    video_source_helper();
    ~video_source_helper();

    // this should be called before any other functions
    void initialize(frame_unit start);

    bool get_samples_end(time_unit request_time, frame_unit& end) const;
    // frame duration must be 1
    void add_new_sample(const media_sample_video_mixer_frame&);
    // updates the last served frame;
    // makes a frame collection up to frame_end
    media_sample_video_mixer_frames_t make_sample(frame_unit frame_end);

    void set_broken(bool broken) {this->broken = broken;}
    void set_initialized(bool fully_initialized) {this->fully_initialized = fully_initialized;}
};