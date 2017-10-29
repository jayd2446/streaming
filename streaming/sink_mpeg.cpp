#include "sink_mpeg.h"
#include <iostream>
#include <Mferror.h>

#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfreadwrite.lib")

//#define LAG_BEHIND 1000000/*(FPS60_INTERVAL * 6)*/
#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

sink_mpeg::sink_mpeg(const media_session_t& session) : 
    media_sink(session), parent(true)
{
    this->processing_callback.Attach(new async_callback_t(&sink_mpeg::processing_cb));

    this->audio_session.reset(new media_session);
    this->loopback_source.reset(new source_loopback(this->audio_session));
    this->loopback_source->initialize();
    this->aac_encoder_transform.reset(new transform_aac_encoder(this->audio_session));
    this->aac_encoder_transform->initialize(this->loopback_source->waveformat_type);
}

sink_mpeg::sink_mpeg(const media_session_t& session, const CComPtr<IMFSinkWriter>& writer) : 
    media_sink(session), parent(false)
{
    this->sink_writer = writer;

    this->processing_callback.Attach(new async_callback_t(&sink_mpeg::processing_cb));
}

sink_mpeg::~sink_mpeg()
{
    if(this->parent)
    {
        this->audio_session->stop_playback();
        this->audio_session->shutdown();

        HRESULT hr = this->sink_writer->Finalize();
        if(FAILED(hr))
            std::cout << "finalizing failed" << std::endl;

        this->sink_writer.Release();
        this->mpeg_media_sink->Shutdown();
    }
}

stream_mpeg_host_t sink_mpeg::create_host_stream(presentation_clock_t& clock)
{
    stream_mpeg_host_t stream(new stream_mpeg_host(this->shared_from_this<sink_mpeg>()));
    stream->register_sink(clock);

    return stream;
}

stream_mpeg_t sink_mpeg::create_worker_stream()
{
    return stream_mpeg_t(new stream_mpeg(this->shared_from_this<sink_mpeg>()));
}

void sink_mpeg::new_packet()
{
    std::unique_lock<std::recursive_mutex> lock(this->writing_mutex, std::try_to_lock);
    if(!lock.owns_lock())
        return;

    std::unique_lock<std::recursive_mutex> packets_lock(this->packets_mutex);
    while(!this->packets.empty())
    {
        packet p;
        auto first_item = this->packets.begin();
        if(!first_item->second.sample_view)
            return;

        p = first_item->second;
        this->packets.erase(first_item);

        packets_lock.unlock();

        // push the sample to the processed samples queue
        scoped_lock lock(this->processed_packets_mutex);
        this->processed_packets.push(p);

        this->processing_cb(NULL);
        // initiate the write
        /*const HRESULT hr = this->processing_callback->mf_put_work_item(
            this->shared_from_this<sink_mpeg>(), MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
        if(FAILED(hr) && hr != MF_E_SHUTDOWN)
            throw std::exception();*/
        packets_lock.lock();
    }
}

void sink_mpeg::processing_cb(void*)
{
    scoped_lock lock(this->processing_mutex);
    packet p;
    {
        scoped_lock lock(this->processed_packets_mutex);
        p = this->processed_packets.front();
        this->processed_packets.pop();
    }

    HRESULT hr = S_OK;
    // send the sample to the sink writer
    media_buffer_samples_t samples = p.sample_view->get_buffer<media_buffer_samples>();
    if(samples)
    {
        /*std::cout << "writing audio..." << std::endl;*/
        for(auto it = samples->samples.begin(); it != samples->samples.end(); it++)
        {
            CHECK_HR(hr = this->sink_writer->WriteSample(
                1, 
                *it));
        }
    }
    else
    {
        /*std::cout << "writing video..." << std::endl;*/
        CHECK_HR(hr = this->sink_writer->WriteSample(
            0,
            p.sample_view->get_buffer<media_buffer_memorybuffer>()->sample));
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

void sink_mpeg::initialize(const CComPtr<IMFMediaType>& input_type)
{
    HRESULT hr = S_OK;
    CComPtr<IMFAttributes> sink_writer_attributes;
    this->mpeg_file_type = input_type;
    this->mpeg_file_type_audio = this->aac_encoder_transform->output_type;

    // create file
    CHECK_HR(hr = MFCreateFile(
        MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, 
        L"test.mp4", &this->byte_stream));

    // create mpeg 4 media sink
    CHECK_HR(hr = MFCreateMPEG4MediaSink(
        this->byte_stream, this->mpeg_file_type, this->mpeg_file_type_audio, &this->mpeg_media_sink));

    // configure the sink writer
    CHECK_HR(hr = MFCreateAttributes(&sink_writer_attributes, 1));
    CHECK_HR(hr = sink_writer_attributes->SetGUID(
        MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));

    // create sink writer
    CHECK_HR(hr = MFCreateSinkWriterFromMediaSink(
        this->mpeg_media_sink, sink_writer_attributes, &this->sink_writer));

    // start accepting data
    CHECK_HR(hr = this->sink_writer->BeginWriting());

    this->mpeg_sink.reset(new sink_mpeg(this->audio_session, this->sink_writer));

done:
    if(FAILED(hr))
        throw std::exception();
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_mpeg::stream_mpeg(const sink_mpeg_t& sink) : sink(sink), available(true)
{
}

media_stream::result_t stream_mpeg::request_sample(request_packet& rp, const media_stream*)
{
    if(!this->sink->session->request_sample(this, rp, true))
    {
        this->available = true;
        return FATAL_ERROR;
    }
    return OK;
}

media_stream::result_t stream_mpeg::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    this->available = true;

    if(!this->sink->session->give_sample(this, sample_view, rp, false))
        return FATAL_ERROR;
    return OK;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_mpeg_host::stream_mpeg_host(const sink_mpeg_t& sink) : 
    sink(sink), 
    running(false),
    unavailable(0)
{
}

void stream_mpeg_host::set_audio_session(time_unit time_point)
{
    presentation_clock_t clock;
    if(!this->get_clock(clock))
        throw std::exception();

    media_topology_t topology(new media_topology(clock->get_time_source()));
    stream_mpeg_host_t mpeg_stream_audio = this->sink->mpeg_sink->create_host_stream(topology->get_clock());
    mpeg_stream_audio->set_pull_rate(48000*1000/1024, 1000*2);

    for(int i = 0; i < WORKER_STREAMS; i++)
    {
        stream_mpeg_t worker_stream_audio = this->sink->mpeg_sink->create_worker_stream();
        media_stream_t aac_encoder_stream = this->sink->aac_encoder_transform->create_stream();
        media_stream_t audio_source_stream = this->sink->loopback_source->create_stream(topology->get_clock());

        mpeg_stream_audio->set_encoder_stream(std::dynamic_pointer_cast<stream_aac_encoder>(aac_encoder_stream));
        mpeg_stream_audio->add_worker_stream(worker_stream_audio);

        topology->connect_streams(audio_source_stream, aac_encoder_stream);
        topology->connect_streams(aac_encoder_stream, worker_stream_audio);
        topology->connect_streams(worker_stream_audio, mpeg_stream_audio);
    }

    if(!this->sink->audio_session->switch_topology_immediate(topology, time_point))
        throw std::exception();
}

bool stream_mpeg_host::on_clock_start(time_unit t)
{
    /*std::cout << "playback started: " << packet_number << ", " << (ptrdiff_t)this << std::endl;*/

    if(!this->encoder_aac_stream)
        this->set_audio_session(t); // switch the topology in the audio session
    this->running = true;
    this->scheduled_callback(t);
    return true;
}

void stream_mpeg_host::on_clock_stop(time_unit t)
{
    /*std::cout << "playback stopped" << ", " << (ptrdiff_t)this << std::endl;*/
    if(this->encoder_aac_stream)
    {
        // TODO: add preroll clock sink notification

        // TODO: set the audio cut off point for the incoming packets
        /*DebugBreak();*/
    }

    this->running = false;
    this->clear_queue();
}

void stream_mpeg_host::scheduled_callback(time_unit due_time)
{
    if(!this->running)
        return;

    // the call order must be this way so that consecutive packet numbers
    // match consecutive request times

    // initiate the request
    // (the initial request call from sink must be synchronized)
    this->dispatch_request(due_time);

    // schedule a new time
    this->schedule_new(due_time);
}

bool bb = true;

void stream_mpeg_host::schedule_new(time_unit due_time)
{
    HRESULT hr = S_OK;

    presentation_clock_t t;
    if(this->get_clock(t))
    {
        if(!bb)
            return;

        const time_unit current_time = t->get_current_time();
        time_unit scheduled_time = this->get_next_due_time(due_time);

        if(!this->schedule_new_callback(scheduled_time))
        {
            if(scheduled_time > current_time)
            {
                // the scheduled time is so close to current time that the callback cannot be set
                std::cout << "VERY CLOSE in sink_mpeg" << std::endl;
                this->scheduled_callback(scheduled_time);
            }
            else
            {
                do
                {
                    const time_unit current_time2 = t->get_current_time();
                    scheduled_time = current_time2;

                    /*if((scheduled_time - LAG_BEHIND) >= 0)
                    {
                        CHECK_HR(hr = this->sink->sink_writer->SendStreamTick(
                            0, scheduled_time - LAG_BEHIND));
                    }*/

                    // at least one frame request was late
                    std::cout << "--------------------------------------------------------------------------------------------" << std::endl;

                    scheduled_time = this->get_next_due_time(scheduled_time);
                }
                // TODO: schedule new callback should be optimized
                while(!this->schedule_new_callback(scheduled_time));
            }
        }
    }

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
}

void stream_mpeg_host::dispatch_request(time_unit request_time)
{
    if(this->unavailable > 240)
    {
        std::cout << "BREAK" << std::endl;
        DebugBreak();
    }

    request_packet rp;
    rp.request_time = request_time;
    rp.timestamp = request_time;

    /*if(request_time >= (SECOND_IN_TIME_UNIT / 2))
    {*/
        scoped_lock lock(this->worker_streams_mutex);
        for(auto it = this->worker_streams.begin(); it != this->worker_streams.end(); it++)
        {
            if((*it)->available)
            {
                this->unavailable = 0;
                (*it)->available = false;
                // increase the packet number and add it to the rp
                rp.packet_number = this->packet_number++;

                // add a packet to the packets queue
                {
                    scoped_lock lock(this->sink->packets_mutex);
                    sink_mpeg::packet p;
                    p.rp = rp;
                    this->sink->packets[rp.request_time] = p;
                }

                // TODO: change fatal error to topology switch;
                // also, subsequent request and give calls cannot fail
                if((*it)->request_sample(rp, this) == FATAL_ERROR)
                    this->running = false;
                else
                    /*std::cout << rp.packet_number << ", " << (ptrdiff_t)this << std::endl*/;
                return;
            }
        }

        std::cout << "--SAMPLE REQUEST DROPPED IN SINK_MPEG--" << std::endl;
        this->unavailable++;
    /*}*/
}

void stream_mpeg_host::add_worker_stream(const stream_mpeg_t& worker_stream)
{
    scoped_lock lock(this->worker_streams_mutex);
    this->worker_streams.push_back(worker_stream);
}

media_stream::result_t stream_mpeg_host::request_sample(request_packet& rp, const media_stream*)
{
    assert(false);
    return OK;
}

media_stream::result_t stream_mpeg_host::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    HRESULT hr = S_OK;

    std::unique_lock<std::recursive_mutex> lock(this->sink->packets_mutex);
    auto it = this->sink->packets.find(rp.request_time);
    assert(it != this->sink->packets.end());

    // TODO: the source should just stall if all the input samples are occupied
    // (or maybe not, because it seems that the mpeg file sink takes the input sample 
    // timestamps into consideration)

    // TODO: this throttles the capture process

    // the encoder will just give an empty texture sample if the input sample was empty
    if(!sample_view || sample_view->get_buffer<media_buffer_texture>())
    {
        this->sink->packets.erase(it);
        /*CHECK_HR(hr = this->sink->sink_writer->SendStreamTick(0, rp.request_time));*/
    }
    else if(sample_view->get_buffer<media_buffer_samples>() || 
        sample_view->get_buffer<media_buffer_memorybuffer>())
    {
        it->second.sample_view = sample_view;
        lock.unlock();
        this->sink->new_packet();
    }
    else
    {
        this->sink->packets.erase(it);
        /*CHECK_HR(hr = this->sink->sink_writer->SendStreamTick(0, rp.request_time));*/
    }

done:
    if(FAILED(hr))
        throw std::exception();
    return OK;
}