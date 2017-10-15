#include "sink_mpeg.h"
#include <iostream>
#include <Mferror.h>

#define LAG_BEHIND 1000000/*(FPS60_INTERVAL * 6)*/
#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

sink_mpeg::sink_mpeg(const media_session_t& session) : media_sink(session), last_packet_number(-1)
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
        if(this->last_packet_number != -1 && first_item->first != (this->last_packet_number + 1))
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
    std::unique_lock<std::recursive_mutex> lock(this->processing_mutex);
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

media_stream::result_t stream_mpeg::request_sample(request_packet& rp)
{
    if(!this->sink->session->request_sample(this, rp, true))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_mpeg::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp)
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
    this->request_callback.Attach(new async_callback_t(&stream_mpeg_host::request_cb));
}

bool stream_mpeg_host::on_clock_start(time_unit t, int packet_number)
{
    std::cout << "playback started" << std::endl;
    this->running = true;
    this->sink->packet_number = packet_number;
    this->scheduled_callback(t);
    return true;
}

void stream_mpeg_host::on_clock_stop(time_unit t)
{
    std::cout << "playback stopped" << std::endl;
    this->running = false;
    this->clear_queue();
}

void stream_mpeg_host::scheduled_callback(time_unit due_time)
{
    if(!this->running)
        return;

    // add the request
    this->push_request(due_time);

    // initiate the request
    const HRESULT hr = this->request_callback->mf_put_work_item(
        this->shared_from_this<stream_mpeg_host>(), MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();

    // schedule a new time
    this->schedule_new(due_time);
}

bool bb = true;

void stream_mpeg_host::schedule_new(time_unit due_time)
{
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

                    // frame request was late
                    std::cout << "--------------------------------------------------------------------------------------------" << std::endl;

                    scheduled_time += pull_interval;
                    scheduled_time -= ((3 * scheduled_time) % 500000) / 3;
                }
                while(!this->schedule_new_callback(scheduled_time));
            }
        }
    }
}

void stream_mpeg_host::push_request(time_unit t)
{
    scoped_lock lock(this->sink->requests_mutex);
    sink_mpeg::request_t request;
    request.request_time = t - LAG_BEHIND;
    request.timestamp = t;
    request.packet_number = this->sink->packet_number++;
    this->sink->requests.push(request);
}

void stream_mpeg_host::request_cb(void*)
{
    sink_mpeg::request_t request;
    request_packet rp;
    {
        scoped_lock lock(this->sink->requests_mutex);
        request = this->sink->requests.front();
        this->sink->requests.pop();
    }

    // wait for the source texture cache to saturate
    if(request.request_time >= 0)
    {
        rp.request_time = request.request_time;
        rp.timestamp = request.timestamp;
        rp.packet_number = request.packet_number;

        if(this->request_sample(rp) == FATAL_ERROR)
            this->running = false;
    }
}

void stream_mpeg_host::add_worker_stream(const stream_mpeg_t& worker_stream)
{
    scoped_lock lock(this->worker_streams_mutex);
    this->worker_streams.push_back(worker_stream);
}

media_stream::result_t stream_mpeg_host::request_sample(request_packet& rp)
{
    // dispatch the request to a worker stream
    std::unique_lock<std::recursive_mutex> lock(this->worker_streams_mutex);
    for(auto it = this->worker_streams.begin(); it != this->worker_streams.end(); it++)
    {
        if((*it)->available)
        {
            (*it)->available = false;
            lock.unlock();
            return (*it)->request_sample(rp);
        }
    }

    std::cout << "--SAMPLE REQUEST DROPPED IN STREAM_MPEG_HOST--" << std::endl;
    return OK;
}

media_stream::result_t stream_mpeg_host::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp)
{
    // the encoder will just give an empty texture sample if the input sample was empty
    if(sample_view->get_sample<media_sample_memorybuffer>())
    {
        {
            scoped_lock lock(this->sink->packets_mutex);
            sink_mpeg::packet p;
            p.rp = rp;
            p.sample_view = sample_view;
            this->sink->packets[rp.packet_number] = p;
        }
        this->sink->new_packet();
    }

    return OK;
}