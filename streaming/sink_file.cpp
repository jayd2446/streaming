#include "sink_file.h"
#include <iostream>

#undef min
#undef max

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

sink_file::sink_file(const media_session_t& session) :
    media_sink(session),
    video(false),
    requests(0),
    last_timestamp(std::numeric_limits<LONGLONG>::min())
{
}

sink_file::~sink_file()
{
}

void sink_file::initialize(const output_file_t& file_output, bool video)
{
    this->file_output = file_output;
    this->video = video;
}

media_stream_t sink_file::create_stream(presentation_clock_t&& clock)
{
    stream_file_t stream(new stream_file(this->shared_from_this<sink_file>()));
    stream->register_sink(clock);

    return stream;
}

void sink_file::process()
{
    scoped_lock lock(this->process_mutex);
    HRESULT hr = S_OK;

    // TODO: dispatch to work queue here instead of just calling the give_sample

    if(this->video)
    {
        request_video_t request;
        while(this->requests_video.pop(request))
        {
            if(request.sample)
            {
                for(auto&& frame : request.sample->sample->frames)
                {
                    const LONGLONG timestamp = (LONGLONG)frame.ts;
                    const LONGLONG dur = (LONGLONG)frame.dur;
                    // (software encoder returns frames slightly in wrong order,
                    // but mediawriter seems to deal with it)
                    if(timestamp <= this->last_timestamp && !request.sample->software)
                    {
                        std::cout << "timestamp error in sink_file::write_cb video" << std::endl;
                        assert_(false);
                    }
                    this->last_timestamp = timestamp;

                    this->file_output->write_sample(true, frame.sample);
                }

                this->session->give_sample(request.stream,
                    request.sample.has_value() ? &(*request.sample) : NULL, request.rp);
            }
        }
    }
    else
    {
        request_audio_t request;
        while(this->requests_audio.pop(request))
        {
            if(request.sample)
            {
                for(auto&& frame : request.sample->sample->frames)
                {
                    const LONGLONG timestamp = (LONGLONG)frame.ts;
                    const LONGLONG dur = (LONGLONG)frame.dur;
                    if(timestamp <= this->last_timestamp)
                    {
                        std::cout << "timestamp error in sink_file::write_cb audio" << std::endl;
                        assert_(false);
                    }
                    this->last_timestamp = timestamp;

                    //sink writer buffers samples
                    this->file_output->write_sample(false, frame.sample);
                }

                this->session->give_sample(request.stream,
                    request.sample.has_value() ? &(*request.sample) : NULL, request.rp);
            }
        }
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_file::stream_file(const sink_file_t& sink) :
    sink(sink),
    media_stream_clock_sink(sink.get())
{
}

void stream_file::on_component_start(time_unit t)
{
}

media_stream::result_t stream_file::request_sample(const request_packet& rp, const media_stream*)
{
    if(this->sink->video)
        this->sink->requests_video.initialize_queue(rp);
    else
        this->sink->requests_audio.initialize_queue(rp);

    return this->sink->session->request_sample(this, rp) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_file::process_sample(
    const media_component_args* args_, const request_packet& rp, const media_stream*)
{
    this->lock();

    if(this->sink->video)
    {
        sink_file::request_video_t request;
        request.rp = rp;
        request.stream = this;
        if(args_)
        {
            const media_component_h264_video_args& args =
                static_cast<const media_component_h264_video_args&>(*args_);
            request.sample = std::make_optional(args);
        }

        // TODO: requests mutex seems unnecessary
        {
            sink_file::scoped_lock lock(this->sink->requests_mutex);
            this->sink->requests_video.push(request);
        }

        // pass null requests downstream
        if(!args_)
            this->sink->session->give_sample(this, NULL, request.rp);
    }
    else
    {
        sink_file::request_audio_t request;
        request.rp = rp;
        request.stream = this;
        if(args_)
        {
            const media_component_aac_audio_args& args =
                static_cast<const media_component_aac_audio_args&>(*args_);
            request.sample = std::make_optional(args);
        }

        {
            sink_file::scoped_lock lock(this->sink->requests_mutex);
            this->sink->requests_audio.push(request);
        }

        // pass null requests downstream
        if(!args_)
            this->sink->session->give_sample(this, NULL, request.rp);
    }

    this->sink->process();
    this->unlock();

    return OK;
}