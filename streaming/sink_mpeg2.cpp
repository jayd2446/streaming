#include "sink_mpeg2.h"
#include <iostream>
#include <Mferror.h>

#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfreadwrite.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

sink_mpeg2::sink_mpeg2(const media_session_t& session) : 
    media_sink(session),
    audio_session(new media_session)
{
    this->write_packets_callback.Attach(new async_callback_t(&sink_mpeg2::write_packets_cb));
}

sink_mpeg2::~sink_mpeg2()
{
    this->audio_session->stop_playback();
    this->audio_session->shutdown();

    HRESULT hr = this->writer->Finalize();
    if(FAILED(hr))
        std::cout << "finalizing failed" << std::endl;

    this->writer.Release();
    this->mpeg_media_sink->Shutdown();
}

void sink_mpeg2::write_packets()
{
    const HRESULT hr = this->write_packets_callback->mf_put_work_item(
        this->shared_from_this<sink_mpeg2>(),
        MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
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

    HRESULT hr = S_OK;
    request_t request;
    while(this->write_queue.pop(request))
    {
        /*std::cout << request.rp.packet_number << std::endl;*/
        media_buffer_memorybuffer_t sample = request.sample_view->get_buffer<media_buffer_memorybuffer>();
        if(!sample)
            continue;
        
        CHECK_HR(hr = this->writer->WriteSample(0, sample->sample));
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

void sink_mpeg2::initialize(
    const CComPtr<IMFMediaType>& video_type, const CComPtr<IMFMediaType>& audio_type)
{
    HRESULT hr = S_OK;
    CComPtr<IMFAttributes> sink_writer_attributes;

    this->video_type = video_type;
    this->audio_type = audio_type;

    // create file
    CHECK_HR(hr = MFCreateFile(
        MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, 
        L"test.mp4", &this->byte_stream));

    // create mpeg 4 media sink
    CHECK_HR(hr = MFCreateMPEG4MediaSink(
        this->byte_stream, this->video_type, this->audio_type, &this->mpeg_media_sink));

    // configure the sink writer
    CHECK_HR(hr = MFCreateAttributes(&sink_writer_attributes, 1));
    CHECK_HR(hr = sink_writer_attributes->SetGUID(
        MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));

    // create sink writer
    CHECK_HR(hr = MFCreateSinkWriterFromMediaSink(
        this->mpeg_media_sink, sink_writer_attributes, &this->writer));

    // start accepting data
    CHECK_HR(hr = this->writer->BeginWriting());

done:
    if(FAILED(hr))
        throw std::exception();
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
}

bool stream_mpeg2::on_clock_start(time_unit t)
{
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

void stream_mpeg2::on_clock_stop(time_unit t)
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
    if(this->unavailable > 240)
        DebugBreak();

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

    if(sample_view->get_buffer<media_buffer_memorybuffer>())
        this->sink->write_packets();

    return OK;
}