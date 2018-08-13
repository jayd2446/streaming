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
        this->shared_from_this<sink_audio>());
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

    request_t request;
    while(this->write_queue.pop(request))
    {
        /*std::cout << request.rp.packet_number << std::endl;*/

        media_buffer_samples_t buffer = request.sample_view.buffer;
        if(!buffer || buffer->samples.empty())
            continue;

        for(auto it = buffer->samples.begin(); it != buffer->samples.end(); it++)
            this->file_output->write_sample(false, *it);
    }
}

void sink_audio::initialize(const output_file_t& file_output)
{
    this->file_output = file_output;
}

stream_audio_t sink_audio::create_stream(presentation_clock_t& clock)
{
    stream_audio_t stream(new stream_audio(this->shared_from_this<sink_audio>()));
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


stream_audio::stream_audio(const sink_audio_t& sink) : 
    sink(sink), unavailable(0), running(false), media_stream_clock_sink(sink.get()), ran_once(false),
    stopped(false)
{
}

void stream_audio::on_stream_start(time_unit t)
{
    //request_packet rp;
    //
    //// discard all the samples that are queued up to this point
    //rp.flags = AUDIO_DISCARD_PREVIOUS_SAMPLES;
    //rp.request_time = t;
    //rp.timestamp = t;
    
    this->running = true;
    this->ran_once = true;
    /*this->dispatch_request(rp);*/
}

void stream_audio::on_stream_stop(time_unit t)
{
    //request_packet rp;

    //// collect all the remaining samples that are queued up to this point
    //rp.flags = 0;
    //rp.request_time = t;
    //rp.timestamp = t;

    //this->dispatch_request(rp);
    this->running = false;
    this->stopped = true;
}

void stream_audio::dispatch_request(request_packet& rp, bool no_drop)
{
    assert_(this->unavailable <= 240);
    /*if(!this->running)
        return;*/

    assert_(this->running);

    const int j = no_drop ? 0 : 1;

    scoped_lock lock(this->worker_streams_mutex);
    for(auto it = this->worker_streams.begin(); it != (this->worker_streams.end() - j); it++)
    {
        if((*it)->available)
        {
            this->unavailable = 0;
            (*it)->available = false;

            result_t res = (*it)->request_sample(rp, this);
            if(res == FATAL_ERROR)
                std::cout << "couldn't dispatch request on stream audio" << std::endl;

            /*assert_(!no_drop || this->stopped);*/

            return;
        }
    }

    // serve the requests from the audio source queue
    /*this->source->serve_requests();*/
    assert_(!no_drop);
    std::cout << "--SAMPLE REQUEST DROPPED IN AUDIO_SINK--" << std::endl;
    this->unavailable++;
}

void stream_audio::add_worker_stream(const stream_audio_worker_t& worker_stream)
{
    scoped_lock lock(this->worker_streams_mutex);
    this->worker_streams.push_back(worker_stream);
}

media_stream::result_t stream_audio::request_sample_last(time_unit t)
{
    request_packet rp;
    rp.request_time = t;
    rp.timestamp = t;

    this->dispatch_request(rp, true);
    return OK;
}

media_stream::result_t stream_audio::request_sample(
    request_packet& rp, const media_stream*)
{
    this->dispatch_request(rp);
    return OK;
}

media_stream::result_t stream_audio::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream*)
{
    const media_sample_aac* audio_sample = dynamic_cast<const media_sample_aac*>(&sample_view);

    // TODO: currently the write queue must be called every time
    // so that it can be properly flushed when stopping recording

    sink_audio::request_t request;
    request.rp = rp;
    if(audio_sample)
        request.sample_view = *audio_sample;
    request.stream = this;
    this->sink->write_queue.push(request);

    this->sink->write_packets();

    return OK;
}