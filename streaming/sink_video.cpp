#include "sink_video.h"
#include "transform_aac_encoder.h"
#include <iostream>
#include <Mferror.h>

#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfreadwrite.lib")

#undef max
#undef min

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

#define AUDIO_BUNDLING_THRESHOLD (SECOND_IN_TIME_UNIT / 50) // 20ms

sink_video::sink_video(const media_session_t& session, const media_session_t& audio_session) : 
    media_sink(session),
    audio_session(audio_session),
    started(false), instant_switch(false)
{
}

sink_video::~sink_video()
{
}

void sink_video::initialize()
{
}

time_unit sink_video::get_audio_pull_periodicity() const
{
    // audio pull periodicity is the length of one aac encoder packet
    // audio_session::frame_rate_num equals to sample rate
    return convert_to_time_unit(1024, 
        this->audio_session->frame_rate_num, this->audio_session->frame_rate_den);
}

void sink_video::switch_topologies(
    const media_topology_t& video_topology,
    const media_topology_t& audio_topology,
    bool instant_switch)
{
    scoped_lock lock(this->topology_switch_mutex);

    assert_(this->is_started());

    this->session->switch_topology(video_topology);
    this->pending_audio_topology = audio_topology;

    if(!this->instant_switch)
        this->instant_switch = instant_switch;
}

void sink_video::start_topologies(
    time_unit t,
    const media_topology_t& video_topology,
    const media_topology_t& audio_topology)
{
    scoped_lock lock(this->topology_switch_mutex);

    assert_(!this->is_started());

    this->session->start_playback(video_topology, t);
    this->audio_session->start_playback(audio_topology, t);

    this->started = true;
}

stream_video_t sink_video::create_stream(
    media_message_generator_t&& message_generator, const stream_audio_t& audio_stream)
{
    stream_video_t stream(
        new stream_video(this->shared_from_this<sink_video>(), audio_stream));
    stream->register_listener(message_generator);

    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


//extern DWORD capture_work_queue_id;

stream_video::stream_video(
    const sink_video_t& sink, const stream_audio_t& audio_sink_stream) : 
    sink(sink), unavailable(0),
    media_stream_message_listener(sink.get()),
    audio_sink_stream(audio_sink_stream),
    stop_point(std::numeric_limits<time_unit>::min()),
    stopping(false),
    discontinuity(false),
    requesting(false),
    requests(0), max_requests(DEFAULT_MAX_REQUESTS),
    video_next_due_time(-1)
{
}

stream_video::~stream_video()
{
}

void stream_video::on_component_start(time_unit)
{
}

void stream_video::on_component_stop(time_unit)
{
}

void stream_video::on_stream_start(time_unit t)
{
    // stream video shouldn't run in higher priority because it will oversaturate the
    // lower priority work queue
    /*this->set_schedule_cb_work_queue(capture_work_queue_id);*/

    this->requesting = true;
    this->video_next_due_time = t;
    this->topology = this->sink->session->get_current_topology();

    this->schedule_new(t);
}

void stream_video::on_stream_stop(time_unit t)
{
    assert_(this->sink->pending_audio_topology);

    this->stopping = true;
    this->stop_point = t;

    if(this->sink->instant_switch)
    {
        this->sink->instant_switch = false;
        this->get_topology()->drained = true;
        this->audio_sink_stream->get_topology()->drained = true;
    }

    // the audio topology will be switched in this call
    this->sink->audio_session->switch_topology(this->sink->pending_audio_topology);
    this->sink->pending_audio_topology = NULL;
}

void stream_video::scheduled_callback(time_unit due_time)
{
    // this lock makes sure that the video and audio topology are switched at the same time
    scoped_lock lock(this->sink->topology_switch_mutex);

    // video might not be pulled as often as audio
    const bool is_video_request = (due_time == this->video_next_due_time);

    request_packet incomplete_rp;
    if(!this->stopping)
        incomplete_rp.request_time = due_time;
    else
    {
        assert_(this->stop_point != std::numeric_limits<time_unit>::min());
        incomplete_rp.request_time = this->stop_point;
    }
    // TODO: make sure that timestamp is properly used in the pipeline
    incomplete_rp.timestamp = due_time;
    incomplete_rp.flags = this->discontinuity ? FLAG_DISCONTINUITY : 0;

    this->discontinuity = false;

    // the call order must be this way so that consecutive packet numbers
    // match consecutive request times
    if(this->requesting && is_video_request)
        this->dispatch_request(incomplete_rp);

    if(this->audio_sink_stream->requesting)
        this->audio_sink_stream->dispatch_request(incomplete_rp);

    // schedule new until both pipelines have stopped requesting
    if(this->requesting || this->audio_sink_stream->requesting)
        this->schedule_new(due_time);

    // TODO: video and audio pipeline should be independently destroyed
    // TODO: this design oddity should be fixed
    // because this stream controls the audio sink, this topology must be destroyed after
    // the audio topology is destroyed
    if(!this->requesting && !this->audio_sink_stream->requesting)
        // the topology must be explicitly set to null so that the circular dependency
        // between the topology and this stream is broken
        this->topology = NULL;
    if(!this->audio_sink_stream->requesting)
        this->audio_sink_stream->topology = NULL;
}

bool stream_video::get_clock(media_clock_t& clock)
{
    /*assert_(this->topology);*/
    clock = this->sink->session->get_clock();
    return !!clock;
}

void stream_video::schedule_new(time_unit due_time)
{
    media_clock_t t;
    const bool ret = this->get_clock(t);
    assert_(ret); ret;

    // update video due time
    if(due_time == this->video_next_due_time)
        this->video_next_due_time = this->get_next_due_time(due_time);

    for(;;)
    {
        time_unit scheduled_time;

        // try bundling the audio due time to video due time
        if((this->video_next_due_time - due_time) < AUDIO_BUNDLING_THRESHOLD)
        {
            assert_(this->video_next_due_time > due_time);
            scheduled_time = this->video_next_due_time;
        }
        else if(std::abs(
            due_time + this->sink->get_audio_pull_periodicity() - this->video_next_due_time) <
            AUDIO_BUNDLING_THRESHOLD)
            scheduled_time = this->video_next_due_time;
        else
            // bundling not possible
            scheduled_time = due_time + this->sink->get_audio_pull_periodicity();

        if(!this->schedule_new_callback<stream_video>(scheduled_time))
        {
            // TODO: discontinuity flag must be separate for audio and video
            this->discontinuity = true;

            // at least one frame request was late
            std::cout << "---------------------------------------------------------------"
                "-----------------------------" << std::endl;

            const time_unit current_time = t->get_current_time();
            this->video_next_due_time = this->get_next_due_time(current_time);
            due_time = current_time;
        }
        else
            // scheduled successfully
            break;
    }
}

void stream_video::dispatch_request(const request_packet& incomplete_rp, bool no_drop)
{
    assert_(this->unavailable <= 240);

    /*static int drops = 0;

    assert_(drops <= 1000);*/

    const int requests = this->requests.load();
    if(requests < this->max_requests || no_drop)
    {
        this->requests++;
        this->unavailable = 0;

        assert_(this->topology);
        this->sink->session->begin_request_sample(this, incomplete_rp, this->topology);
    }
    else
    {
        this->discontinuity = true;
        this->unavailable++;

        std::cout << "--SAMPLE REQUEST DROPPED IN VIDEO_SINK--";
        if(this->encoder_stream && this->encoder_stream->get_transform()->is_encoder_overloading())
            std::cout << " (encoder overloading)";
        std::cout << std::endl;

        /*drops++;*/
    }
}

media_stream::result_t stream_video::request_sample(const request_packet& rp, const media_stream*)
{
    if(rp.flags & FLAG_LAST_PACKET)
    {
        assert_(this->stopping);
        this->requesting = false;
    }
    /*if(this->stopping && this->sink->session->is_drainable(this->stop_point, rp.topology))
        this->requesting = false;*/

    if(!this->sink->session->request_sample(this, rp))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_video::process_sample(
    const media_component_args*, const request_packet&, const media_stream*)
{
    // TODO: request count should be dropped only after the request packet has been destroyed

    // multithreaded

    this->requests--;

    return OK;
}