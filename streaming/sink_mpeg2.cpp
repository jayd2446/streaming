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
    presentation_clock_t&& clock, const stream_audio_t& audio_stream)
{
    stream_mpeg2_t stream(
        new stream_mpeg2(this->shared_from_this<sink_mpeg2>(), audio_stream));
    stream->register_sink(clock);

    return stream;
}

stream_worker_t sink_mpeg2::create_worker_stream()
{
    return stream_worker_t(new stream_worker(this->shared_from_this<sink_mpeg2>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


extern DWORD capture_work_queue_id;

stream_mpeg2::stream_mpeg2(
    const sink_mpeg2_t& sink, const stream_audio_t& audio_sink_stream) : 
    sink(sink), unavailable(0), running(false),
    media_stream_clock_sink(sink.get()),
    audio_sink_stream(audio_sink_stream),
    last_due_time(std::numeric_limits<time_unit>::min()),
    discontinuity(false)
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
    this->set_schedule_cb_work_queue(capture_work_queue_id);

    this->running = true;
    this->last_due_time = t;
    this->schedule_new(t);
}

void stream_mpeg2::on_stream_stop(time_unit t)
{
    this->running = false;
    this->clear_queue();

    // the audio topology will be switched in this call
    assert_(this->sink->pending_audio_topology);
    this->sink->audio_session->switch_topology(this->sink->pending_audio_topology);
    this->sink->pending_audio_topology = NULL;

    this->audio_sink_stream->request_sample_last(t);
}

void stream_mpeg2::scheduled_callback(time_unit due_time)
{
    // this lock makes sure that the video and audio topology are
    // switched at the same time
    scoped_lock lock(this->sink->topology_switch_mutex);

    if(!this->running)
        return;

    request_packet rp, rp2;
    rp.request_time = due_time;
    rp.timestamp = due_time;
    rp.flags = this->discontinuity ? FLAG_DISCONTINUITY : 0;
    rp2 = rp;

    this->discontinuity = false;

    // the call order must be this way so that consecutive packet numbers
    // match consecutive request times
    this->dispatch_request(rp);
    if(this->running)
    {
        using namespace std::chrono;
        time_unit_t t2(this->last_due_time), t(due_time);
        const sink_audio::periodicity_t periodicity =
            duration_cast<sink_audio::periodicity_t>(t - t2);

        if(periodicity.count())
        {
            this->last_due_time = due_time;
            this->audio_sink_stream->request_sample(rp2);
        }
        this->schedule_new(due_time);
    }
}

void stream_mpeg2::schedule_new(time_unit due_time)
{
    presentation_clock_t t;
    if(this->get_clock(t))
    {
        const time_unit current_time = t->get_current_time();
        time_unit scheduled_time = this->get_next_due_time(due_time);

        if(!this->schedule_new_callback(scheduled_time))
        {
            if(scheduled_time > current_time)
            {
                std::cout << "VERY CLOSE in stream_mpeg2" << std::endl;
                this->scheduled_callback(scheduled_time);
            }
            else
            {
                this->discontinuity = true;
                do
                {
                    const time_unit current_time = t->get_current_time();
                    scheduled_time = current_time;

                    // at least one frame request was late
                    std::cout << "--------------------------------------------------------------------------------------------" << std::endl;
                    scheduled_time = this->get_next_due_time(scheduled_time);
                }
                while(!this->schedule_new_callback(scheduled_time));
            }
        }
    }
}

void stream_mpeg2::dispatch_request(request_packet& rp, bool no_drop)
{
    assert_(this->unavailable <= 240);

    // initiate the video request
    scoped_lock lock(this->worker_streams_mutex);
    for(auto it = this->worker_streams.begin(); it != this->worker_streams.end(); it++)
    {
        if((*it)->is_available())
        {
            /*this->unavailable = 0;
            (*it)->available = false;*/

            result_t res = (*it)->request_sample(rp, this);
            if(res == FATAL_ERROR)
                std::cout << "couldn't dispatch request on stream mpeg" << std::endl;
            return;
        }
    }

    this->discontinuity = true;

    // TODO: remove no drop since clock start and clock stop requests cannot
    // be dropped anymore
    if(no_drop)
        std::cout << "TODO: NO DROP REQUEST DROPPED ";
    std::cout << "--SAMPLE REQUEST DROPPED IN MPEG_SINK--" << std::endl;
    this->unavailable++;
}

void stream_mpeg2::add_worker_stream(const stream_worker_t& worker_stream)
{
    scoped_lock lock(this->worker_streams_mutex);
    this->worker_streams.push_back(worker_stream);
}

media_stream::result_t stream_mpeg2::process_sample(
    const media_sample&, request_packet&, const media_stream*)
{
    return OK;
}