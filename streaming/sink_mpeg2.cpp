#include "sink_mpeg2.h"
#include <iostream>
#include <Mferror.h>

#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfreadwrite.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

sink_mpeg2::sink_mpeg2(const media_session_t& session, const media_session_t& audio_session) : 
    media_sink(session),
    audio_session(audio_session),
    /*audio_session(new media_session(session->get_time_source())),*/
    stopped_signal(NULL)
{
    this->write_packets_callback.Attach(new async_callback_t(&sink_mpeg2::write_packets_cb));
}

sink_mpeg2::~sink_mpeg2()
{
    /*this->audio_session->stop_playback();
    this->audio_session->shutdown();*/
}

void sink_mpeg2::write_packets()
{
    const HRESULT hr = this->write_packets_callback->mf_put_work_item(
        this->shared_from_this<sink_mpeg2>());
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
    else if(hr == MF_E_SHUTDOWN)
        return;
}

void sink_mpeg2::write_packets_cb(void*)
{
    std::unique_lock<std::recursive_mutex> lock(this->writing_mutex, std::try_to_lock);
    if(!lock.owns_lock())
        return;

    request_t request;
    while(this->write_queue.pop(request))
    {
        /*std::cout << request.rp.packet_number << std::endl;*/
        if(!request.sample_view)
            continue;

        media_buffer_memorybuffer_t sample = request.sample_view->get_buffer<media_buffer_memorybuffer>();
        if(!sample)
            continue;
        
        this->file_output->write_sample(true, sample->sample);
    }
}

void sink_mpeg2::initialize(
    bool null_file,
    HANDLE stopped_signal,
    const CComPtr<IMFMediaType>& video_type, const CComPtr<IMFMediaType>& audio_type)
{
    this->file_output.reset(new output_file);
    this->file_output->initialize(null_file, stopped_signal, video_type, audio_type);
}

void sink_mpeg2::set_new_audio_topology(const stream_audio_t& audio_sink_stream,
    const media_topology_t& audio_topology)
{
    std::atomic_exchange(&this->new_audio_sink_stream, audio_sink_stream);
    std::atomic_exchange(&this->new_audio_topology, audio_topology);
}

stream_mpeg2_t sink_mpeg2::create_stream(presentation_clock_t& clock)
{
    stream_mpeg2_t stream(new stream_mpeg2(this->shared_from_this<sink_mpeg2>()));
    stream->register_sink(clock);

    return stream;
}

stream_mpeg2_worker_t sink_mpeg2::create_worker_stream()
{
    return stream_mpeg2_worker_t(new stream_mpeg2_worker(this->shared_from_this<sink_mpeg2>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_mpeg2::stream_mpeg2(const sink_mpeg2_t& sink) : sink(sink), unavailable(0), running(false)
{
    HRESULT hr = S_OK;
    DWORD task_id;
    CHECK_HR(hr = MFLockSharedWorkQueue(L"Capture", 0, &task_id, &this->work_queue_id));

done:
    if(FAILED(hr))
        throw std::exception();
}

stream_mpeg2::~stream_mpeg2()
{
    MFUnlockWorkQueue(this->work_queue_id);
}

bool stream_mpeg2::on_clock_start(time_unit t)
{
    this->set_schedule_cb_work_queue(this->work_queue_id);

    // try to set the initial time for the output;
    // the output will modify the sample timestamps so that they start at 0
    this->sink->get_output()->set_initial_time(t);

    // set the new audio topology for the audio session
    media_topology_t new_audio_topology = std::atomic_load(&this->sink->new_audio_topology);
    std::atomic_exchange(&this->sink->new_audio_topology, media_topology_t());
    if(new_audio_topology)
    {
        // load the new audio sink stream aswell
        std::atomic_exchange(&this->sink->audio_sink_stream, 
            std::atomic_load(&this->sink->new_audio_sink_stream));
        std::atomic_exchange(&this->sink->new_audio_sink_stream, stream_audio_t());

        // this causes the audio session to fire on_clock_start events;
        // the dispatch_request might cause audio sink to fetch samples with the same
        // time point, which will return null sample since the same time point
        // was used to discard all the samples
        this->sink->audio_session->switch_topology_immediate(new_audio_topology, t);
    }

    this->running = true;
    this->scheduled_callback(t);
    return true;
}

void stream_mpeg2::on_clock_stop(time_unit)
{
    // TODO: audio session should be stopped here aswell
    this->running = false;
    this->clear_queue();
}

void stream_mpeg2::scheduled_callback(time_unit due_time)
{
    if(!this->running)
        return;

    // the call order must be this way so that consecutive packet numbers
    // match consecutive request times
    request_packet rp;
    rp.request_time = due_time;
    rp.timestamp = due_time;

    this->dispatch_request(rp);
    this->schedule_new(due_time);
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

void stream_mpeg2::dispatch_request(request_packet& rp)
{
    assert_(this->unavailable <= 240);

    // initiate the audio request
    // TODO: this if statement doesn't work for all pull rates
    request_packet rp2 = rp;
    if((rp2.request_time % SECOND_IN_TIME_UNIT) == 0)
        this->sink->audio_sink_stream->request_sample(rp2);

    // initiate the video request
    scoped_lock lock(this->worker_streams_mutex);
    for(auto it = this->worker_streams.begin(); it != this->worker_streams.end(); it++)
    {
        if((*it)->available)
        {
            this->unavailable = 0;
            (*it)->available = false;

            result_t res = (*it)->request_sample(rp, this);
            if(res == FATAL_ERROR)
                std::cout << "topology switched" << std::endl;
            return;
        }
    }

    std::cout << "--SAMPLE REQUEST DROPPED IN MPEG_SINK--" << std::endl;
    this->unavailable++;
}

void stream_mpeg2::add_worker_stream(const stream_mpeg2_worker_t& worker_stream)
{
    scoped_lock lock(this->worker_streams_mutex);
    this->worker_streams.push_back(worker_stream);
}

media_stream::result_t stream_mpeg2::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    sink_audio::request_t request;
    request.rp = rp;
    request.sample_view = sample_view;
    request.stream = this;
    this->sink->write_queue.push(request);

    // if the sample view is locking a resource,
    // it must be ensured that the resource is unlocked in this call;
    // currently though, the sample views won't lock at this point anymore

    this->sink->write_packets();

    return OK;
}