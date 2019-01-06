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
    sink(sink), unavailable(0), /*running(false),*/
    media_stream_clock_sink(sink.get()),
    audio_sink_stream(audio_sink_stream),
    last_due_time(std::numeric_limits<time_unit>::min()),
    stop_point(std::numeric_limits<time_unit>::min()),
    discontinuity(false),
    requesting(false), processing(false),
    requests(0), max_requests(VIDEO_MAX_REQUESTS)
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

    this->requesting = this->processing = true;

    /*this->running = true;*/
    this->last_due_time = t;
    this->schedule_new(t);
}

void stream_mpeg2::on_stream_stop(time_unit t)
{
    // on stream stop event the rp that has the same time point
    // will be passed upstream and not dropped

    this->requesting = false;
    this->stop_point = t;

    /*this->running = false;
    this->clear_queue();*/

    // TODO: the time point for the last audio request could be modified so that
    // the audio switch will happen after the last video frame has been presented;
    // currently the audio will switch virtually at the same time when the last video
    // frame is presented

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

    /*if(!this->running)
        return;*/

    request_packet incomplete_rp;
    incomplete_rp.request_time = due_time;
    incomplete_rp.timestamp = due_time;
    incomplete_rp.flags = this->discontinuity ? FLAG_DISCONTINUITY : 0;

    this->discontinuity = false;

    // the call order must be this way so that consecutive packet numbers
    // match consecutive request times
    if(this->requesting)
        this->dispatch_request(incomplete_rp);
    if(this->processing)
        this->dispatch_process();

    if(this->requesting)
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
    }

    if(this->processing)
        this->schedule_new(due_time);

    if(!this->processing)
        // the topology must be explicitly set to null so that the circular dependency
        // between the topology and this stream is broken
        this->topology = NULL;
}

void stream_mpeg2::schedule_new(time_unit due_time)
{
    presentation_clock_t t;
    if(this->get_clock(t))
    {
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


        //if(!this->schedule_new_callback<stream_mpeg2>(scheduled_time))
        //{
        //    this->discontinuity = true;
        //    do
        //    {
        //        const time_unit current_time = t->get_current_time();
        //        scheduled_time = current_time;

        //        // at least one frame request was late
        //        std::cout << "--------------------------------------------------------------------------------------------" << std::endl;
        //        scheduled_time = this->get_next_due_time(scheduled_time);
        //    }
        //    while(!this->schedule_new_callback<stream_mpeg2>(scheduled_time));
        //}
    }
}

void stream_mpeg2::dispatch_request(const request_packet& incomplete_rp, bool no_drop)
{
    assert_(this->unavailable <= 240);

    // initiate the video request
    scoped_lock lock(this->worker_streams_mutex);

    const int requests = this->requests.load();
    if(requests < this->max_requests)
    {
        this->requests++;
        this->unavailable = 0;

        this->topology = this->sink->session->begin_request_sample(this, incomplete_rp);
        assert_(this->topology);

        /*const result_t res = this->worker_stream->request_sample(rp, this);
        if(res == FATAL_ERROR)
            std::cout << "couldn't dispatch request on stream mpeg" << std::endl;
        else
        {
            assert_(rp.topology);
            if(!this->topology)
                this->topology = rp.topology;
        }*/
    }
    else
    {
        /*for(auto it = this->worker_streams.begin(); it != this->worker_streams.end(); it++)
        {
            if((*it)->is_available())
            {
                this->unavailable = 0;

                result_t res = (*it)->request_sample(rp, this);
                if(res == FATAL_ERROR)
                    std::cout << "couldn't dispatch request on stream mpeg" << std::endl;
                else
                {
                    assert_(rp.topology);
                    if(!this->topology)
                        this->topology = rp.topology;
                }

                return;
            }
        }*/

        this->discontinuity = true;

        // TODO: remove no drop since clock start and clock stop requests cannot
        // be dropped anymore
        if(no_drop)
            std::cout << "TODO: NO DROP REQUEST DROPPED ";
        std::cout << "--SAMPLE REQUEST DROPPED IN MPEG_SINK--" << std::endl;
        this->unavailable++;
    }
}

void stream_mpeg2::dispatch_process()
{
    // TODO: schedule a new after dispatching

    // TODO: this should dispatch to a work queue for each source for
    // better performance(or maybe not)
    // TODO: each source should check if there's an rp to process;
    // source wasapi probably doesn't need to create a dummy rendering stream after
    // the refactoring(but it still should)

    // the dispatch process is a separate mechanic from the request/process pair

    // stream mpeg2 should stop running after the last request has been satisfied;
    // stream mpeg2 cannot make new requests after the stream stop event, but
    // it will keep processing the requests until the last request has been satisfied
    // (this also implies that the processing will be a different mechanic)

    // TODO: sink_mpeg should be at the root of the process topology

    // TODO: decide how to refactor this;
    // it could be refactored by making request_sample&give_sample its own function
    // where the is_sink/is_source flags are implicated;
    // for request_sample, request packet can be omitted while
    // for give_sample sample and request packet can be omitted
    this->sink->session->begin_give_sample(this, this->topology);
}

void stream_mpeg2::add_worker_stream(const stream_worker_t& worker_stream)
{
    scoped_lock lock(this->worker_streams_mutex);

    worker_stream->set_max_requests(VIDEO_MAX_REQUESTS);
    worker_stream->not_used();

    this->worker_stream = worker_stream;
}

media_stream::result_t stream_mpeg2::process_sample(
    const media_sample&, request_packet& rp, const media_stream*)
{
    this->requests--;

    // the last request has been processed;
    // stop further processing
    if(rp.request_time == this->stop_point)
        this->processing = false;

    return OK;
}