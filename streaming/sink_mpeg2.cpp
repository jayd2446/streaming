#include "sink_mpeg2.h"
#include <iostream>
#include <Mferror.h>

#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfreadwrite.lib")

#undef max
#undef min

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

sink_mpeg2::sink_mpeg2(const media_session_t& session, const media_session_t& audio_session) : 
    media_sink(session),
    audio_session(audio_session),
    started(false)
{
}

sink_mpeg2::~sink_mpeg2()
{
}

void sink_mpeg2::initialize()
{
}

void sink_mpeg2::switch_topologies(
    const media_topology_t& video_topology,
    const media_topology_t& audio_topology)
{
    scoped_lock lock(this->topology_switch_mutex);

    assert_(this->is_started());

    this->session->switch_topology(video_topology);
    this->pending_audio_topology = audio_topology;
}

void sink_mpeg2::start_topologies(
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

stream_mpeg2_t sink_mpeg2::create_stream(
    media_message_generator_t&& message_generator, const stream_audio_t& audio_stream)
{
    stream_mpeg2_t stream(
        new stream_mpeg2(this->shared_from_this<sink_mpeg2>(), audio_stream));
    stream->register_listener(message_generator);

    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


extern DWORD capture_work_queue_id;

stream_mpeg2::stream_mpeg2(
    const sink_mpeg2_t& sink, const stream_audio_t& audio_sink_stream) : 
    sink(sink), unavailable(0), /*running(false),*/
    media_stream_message_listener(sink.get()),
    audio_sink_stream(audio_sink_stream),
    stop_point(std::numeric_limits<time_unit>::min()),
    stopping(false),
    discontinuity(false),
    requesting(false),
    requests(0), max_requests(DEFAULT_MAX_REQUESTS)
{
}

stream_mpeg2::~stream_mpeg2()
{
}

void stream_mpeg2::on_component_start(time_unit)
{
}

void stream_mpeg2::on_component_stop(time_unit)
{
}

void stream_mpeg2::on_stream_start(time_unit t)
{
    // stream mpeg2 shouldn't run in higher priority because it will oversaturate the
    // lower priority work queue
    /*this->set_schedule_cb_work_queue(capture_work_queue_id);*/

    this->requesting = true;

    this->topology = this->sink->session->get_current_topology();

    /*this->running = true;*/
    this->schedule_new(t);
}

void stream_mpeg2::on_stream_stop(time_unit t)
{
    /*this->requesting = false;*/
    this->stopping = true;
    this->stop_point = t;

    /*this->running = false;
    this->clear_queue();*/

    // the audio topology will be switched in this call
    assert_(this->sink->pending_audio_topology);
    this->sink->audio_session->switch_topology(this->sink->pending_audio_topology);
    this->sink->pending_audio_topology = NULL;
}

void stream_mpeg2::scheduled_callback(time_unit due_time)
{
    // this lock makes sure that the video and audio topology are switched at the same time
    scoped_lock lock(this->sink->topology_switch_mutex);

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
    if(this->requesting)
        this->dispatch_request(incomplete_rp);
    // TODO: performance might be increased if audio is processed in batches of
    // aac encoder frame granularity;
    // it might increase the performance of the algorithms as well as it decreases
    // the overhead of the pipeline;
    // TODO: restore the audio pipeline periodicity
    if(this->audio_sink_stream->requesting)
        this->audio_sink_stream->dispatch_request(incomplete_rp);
    /*if(this->processing)
        this->dispatch_process();
    if(this->audio_sink_stream->processing)
        this->audio_sink_stream->dispatch_process();*/

    /*if(this->requesting)
    {
        using namespace std::chrono;
        time_unit_t t2(this->last_due_time), t(due_time);
        const sink_audio::periodicity_t periodicity =
            duration_cast<sink_audio::periodicity_t>(t - t2);

        if(periodicity.count())
        {
            this->last_due_time = due_time;
            this->audio_sink_stream->request_sample(incomplete_rp);
        }
    }*/

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

bool stream_mpeg2::get_clock(media_clock_t& clock)
{
    /*assert_(this->topology);*/
    clock = this->sink->session->get_clock();
    return !!clock;
}

void stream_mpeg2::schedule_new(time_unit due_time)
{
    media_clock_t t;
    const bool ret = this->get_clock(t);
    assert_(ret);

    time_unit scheduled_time = this->get_next_due_time(due_time);
    while(!this->schedule_new_callback<stream_mpeg2>(scheduled_time))
    {
        this->discontinuity = true;

        const time_unit current_time = t->get_current_time();
        scheduled_time = current_time;

        // at least one frame request was late
        std::cout << "--------------------------------------------------------------------------------------------" << std::endl;
        scheduled_time = this->get_next_due_time(scheduled_time);
    }
}

void stream_mpeg2::dispatch_request(const request_packet& incomplete_rp, bool no_drop)
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

        std::cout << "--SAMPLE REQUEST DROPPED IN MPEG_SINK--";
        if(this->encoder_stream && this->encoder_stream->get_transform()->is_encoder_overloading())
            std::cout << " (encoder overloading)";
        std::cout << std::endl;

        /*drops++;*/
    }
}

media_stream::result_t stream_mpeg2::request_sample(const request_packet& rp, const media_stream*)
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

media_stream::result_t stream_mpeg2::process_sample(
    const media_component_args*, const request_packet&, const media_stream*)
{
    // TODO: request count should be dropped only after the request packet has been destroyed

    // multithreaded

    this->requests--;

    return OK;
}