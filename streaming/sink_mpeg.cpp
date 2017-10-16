#include "sink_mpeg.h"
#include <iostream>
#include <Mferror.h>

#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfreadwrite.lib")

#define LAG_BEHIND 1000000/*(FPS60_INTERVAL * 6)*/
#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

sink_mpeg::sink_mpeg(const media_session_t& session) : media_sink(session)
{
    this->processing_callback.Attach(new async_callback_t(&sink_mpeg::processing_cb));
}

sink_mpeg::~sink_mpeg()
{
    HRESULT hr = this->sink_writer->Finalize();
    if(FAILED(hr))
        std::cout << "finalizing failed" << std::endl;

    this->sink_writer.Release();
    this->mpeg_media_sink->Shutdown();
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
    scoped_lock lock(this->packets_mutex);
    while(!this->packets.empty())
    {
        packet p;
        auto first_item = this->packets.begin();
        if(!first_item->second.sample_view)
            return;

        p = first_item->second;
        this->packets.erase(first_item);

        // push the sample to the processed samples queue
        scoped_lock lock(this->processed_packets_mutex);
        this->processed_packets.push(p);

        // initiate the write
        const HRESULT hr = this->processing_callback->mf_put_work_item(
            this->shared_from_this<sink_mpeg>(), MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
        if(FAILED(hr) && hr != MF_E_SHUTDOWN)
            throw std::exception();
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
    CHECK_HR(hr = this->sink_writer->WriteSample(
        0, 
        p.sample_view->get_sample<media_sample_memorybuffer>()->sample));

done:
    if(FAILED(hr))
        throw std::exception();
}

void sink_mpeg::initialize(const CComPtr<IMFMediaType>& input_type)
{
    HRESULT hr = S_OK;
    CComPtr<IMFAttributes> sink_writer_attributes;
    this->mpeg_file_type = input_type;

    // create file
    CHECK_HR(hr = MFCreateFile(
        MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, 
        L"test.mp4", &this->byte_stream));

    // create mpeg 4 media sink
    CHECK_HR(hr = MFCreateMPEG4MediaSink(
        this->byte_stream, this->mpeg_file_type, NULL, &this->mpeg_media_sink));

    // configure the sink writer
    CHECK_HR(hr = MFCreateAttributes(&sink_writer_attributes, 1));
    CHECK_HR(hr = sink_writer_attributes->SetGUID(
        MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));

    // create sink writer
    CHECK_HR(hr = MFCreateSinkWriterFromMediaSink(
        this->mpeg_media_sink, sink_writer_attributes, &this->sink_writer));

    // start accepting data
    CHECK_HR(hr = this->sink_writer->BeginWriting());

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


stream_mpeg_host::stream_mpeg_host(const sink_mpeg_t& sink) : sink(sink), running(false)
{
}

bool stream_mpeg_host::on_clock_start(time_unit t, int packet_number)
{
    /*std::cout << "playback started: " << packet_number << ", " << (ptrdiff_t)this << std::endl;*/
    this->running = true;
    this->packet_number = packet_number;
    this->scheduled_callback(t);
    return true;
}

void stream_mpeg_host::on_clock_stop(time_unit t)
{
    /*std::cout << "playback stopped" << ", " << (ptrdiff_t)this << std::endl;*/
    this->running = false;
    this->clear_queue();
}

void stream_mpeg_host::scheduled_callback(time_unit due_time)
{
    if(!this->running)
        return;

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

        const time_unit pull_interval = 166667;
        const time_unit current_time = t->get_current_time();
        time_unit scheduled_time = due_time;

        scheduled_time += pull_interval;
        scheduled_time -= ((3 * scheduled_time) % 500000) / 3;

        if(!this->schedule_new_callback(scheduled_time))
        {
            /*
TODO: schedule_new can retroactively dispatch a request even if the calculated scheduled time
has already been surpassed(this means that the lag behind constant is between the max and min of
sample timestamps in the source)
            */
            if(scheduled_time > current_time)
            {
                // the scheduled time is so close to current time that the callback cannot be set
                std::cout << "VERY CLOSE in sink_mpeg" << std::endl;
                this->scheduled_callback(scheduled_time);
            }
            else
            {
                // TODO: calculate here how many frame requests missed
                do
                {
                    // this commented line will skip the loop and calculate the
                    // next frame
                    /*const time_unit current_time2 = t->get_current_time();
                    scheduled_time = current_time2;*/

                    if((scheduled_time - LAG_BEHIND) >= 0)
                    {
                        CHECK_HR(hr = this->sink->sink_writer->SendStreamTick(
                            0, scheduled_time - LAG_BEHIND));
                    }

                    // frame request was late
                    std::cout << "--------------------------------------------------------------------------------------------" << std::endl;

                    scheduled_time += pull_interval;
                    scheduled_time -= ((3 * scheduled_time) % 500000) / 3;
                }
                while(!this->schedule_new_callback(scheduled_time));
            }
        }
    }

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
}

void stream_mpeg_host::dispatch_request(time_unit due_time)
{
    const time_unit request_time = due_time - LAG_BEHIND;

    // let the source texture queue saturate a bit
    if(request_time >= 0)
    {
        request_packet rp;
        rp.request_time = request_time;
        rp.timestamp = due_time;

        scoped_lock lock(this->worker_streams_mutex);
        for(auto it = this->worker_streams.begin(); it != this->worker_streams.end(); it++)
        {
            if((*it)->available)
            {
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
    }
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
    //// dispatch the request to a worker stream
    //std::unique_lock<std::recursive_mutex> lock(this->worker_streams_mutex);
    //for(auto it = this->worker_streams.begin(); it != this->worker_streams.end(); it++)
    //{
    //    if((*it)->available)
    //    {
    //        (*it)->available = false;
    //        lock.unlock();
    //        
    //        // deadlock happens because the available property is set
    //        // even if there's a sample on a queue; the next request will thus wait,
    //        // and a deadlock will occur because the packet numbers aren't consecutive anymore

    //        // add a packet to the packets queue
    //        std::unique_lock<std::recursive_mutex> lock(this->sink->packets_mutex);
    //        sink_mpeg::packet p;
    //        p.rp = rp;
    //        this->sink->packets[rp.request_time] = p;
    //        lock.unlock();

    //        /*std::cout << rp.packet_number << std::endl;*/

    //        return (*it)->request_sample(rp, this);
    //    }
    //}

    //std::cout << "--SAMPLE REQUEST DROPPED IN STREAM_MPEG_HOST--" << std::endl;
    //return OK;
}

media_stream::result_t stream_mpeg_host::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    std::unique_lock<std::recursive_mutex> lock(this->sink->packets_mutex);
    auto it = this->sink->packets.find(rp.request_time);
    assert(it != this->sink->packets.end());

    // the encoder will just give an empty texture sample if the input sample was empty
    if(sample_view->get_sample<media_sample_memorybuffer>())
    {
        it->second.sample_view = sample_view;
        lock.unlock();
        this->sink->new_packet();
    }
    else
        this->sink->packets.erase(it);

    return OK;
}