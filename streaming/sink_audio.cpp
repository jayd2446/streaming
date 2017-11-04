#include "sink_audio.h"
#include <iostream>
#include <Mferror.h>

#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfreadwrite.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

sink_audio::sink_audio(const media_session_t& session) : media_sink(session)
{
    this->write_packets_callback.Attach(new async_callback_t(&sink_audio::write_packets_cb));
}

void sink_audio::write_packets()
{
    const HRESULT hr = this->write_packets_callback->mf_put_work_item(
        this->shared_from_this<sink_audio>(),
        MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
    else if(hr == MF_E_SHUTDOWN)
        return;
}

void sink_audio::write_packets_cb(void*)
{
    std::unique_lock<std::recursive_mutex> lock(this->writing_mutex, std::try_to_lock);
    if(!lock.owns_lock())
        return;

    HRESULT hr = S_OK;
    request_t request;
    while(this->write_queue.pop(request))
    {
        /*std::cout << request.rp.packet_number << std::endl;*/
        if(!request.sample_view)
            continue;

        media_buffer_samples_t samples = request.sample_view->get_buffer<media_buffer_samples>();
        for(auto it = samples->samples.begin(); it != samples->samples.end(); it++)
            CHECK_HR(hr = this->writer->WriteSample(1, *it));
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

void sink_audio::initialize(const CComPtr<IMFSinkWriter>& writer)
{
    this->writer = writer;
}

stream_audio_t sink_audio::create_stream(presentation_clock_t& clock, const source_loopback_t& source)
{
    stream_audio_t stream(new stream_audio(this->shared_from_this<sink_audio>(), source));
    stream->register_sink(clock);

    return stream;
}

stream_audio_worker_t sink_audio::create_worker_stream()
{
    return stream_audio_worker_t(new stream_audio_worker(this->shared_from_this<sink_audio>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_audio::stream_audio(const sink_audio_t& sink, const source_loopback_t& source) : 
    sink(sink), source(source), unavailable(0)
{
}

bool stream_audio::on_clock_start(time_unit t)
{
    request_packet rp;
    
    // discard all the samples that are queued up to this point
    rp.flags = AUDIO_DISCARD_PREVIOUS_SAMPLES;
    rp.request_time = t;
    rp.timestamp = t;
    
    this->dispatch_request(rp);
    return true;
}

void stream_audio::on_clock_stop(time_unit t)
{
    request_packet rp;

    // collect all the remaining samples that are queued up to this point
    rp.flags = 0;
    rp.request_time = t;
    rp.timestamp = t;

    this->dispatch_request(rp);
}

void stream_audio::dispatch_request(request_packet& rp)
{
    if(this->unavailable > 240)
        DebugBreak();

    scoped_lock lock(this->worker_streams_mutex);
    for(auto it = this->worker_streams.begin(); it != this->worker_streams.end(); it++)
    {
        if((*it)->available)
        {
            this->unavailable = 0;
            (*it)->available = false;

            result_t res = (*it)->request_sample(rp, this);
            // serve the requests from the audio source queue
            /*this->source->serve_requests();*/
            return;
        }
    }

    // serve the requests from the audio source queue
    /*this->source->serve_requests();*/
    std::cout << "--SAMPLE REQUEST DROPPED IN AUDIO_SINK--" << std::endl;
    this->unavailable++;
}

void stream_audio::add_worker_stream(const stream_audio_worker_t& worker_stream)
{
    scoped_lock lock(this->worker_streams_mutex);
    this->worker_streams.push_back(worker_stream);
}

media_stream::result_t stream_audio::request_sample(
    request_packet& rp, const media_stream*)
{
    rp.flags = 0;
    this->dispatch_request(rp);
    return OK;
}

media_stream::result_t stream_audio::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    // TODO: this must be fixed
    sink_audio::request_t request;
    request.rp = rp;
    request.sample_view = sample_view;
    request.stream = this;
    this->sink->write_queue.push(request);

    if(sample_view)
        this->sink->write_packets();

    return OK;
}