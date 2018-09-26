#include "sink_file.h"
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

sink_file::sink_file(const media_session_t& session) :
    media_sink(session),
    work_queue_id(0),
    video(false),
    requests(0),
    last_timestamp(std::numeric_limits<LONGLONG>::min())
{
    this->write_callback.Attach(new async_callback_t(&sink_file::write_cb));
}

sink_file::~sink_file()
{
    HRESULT hr = MFUnlockWorkQueue(this->work_queue_id);
}

void sink_file::initialize(const output_file_t& file_output, bool video)
{
    this->file_output = file_output;
    this->video = video;

    HRESULT hr = S_OK;
    CHECK_HR(hr = MFAllocateSerialWorkQueue(
        MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &this->work_queue_id));
    this->write_callback->native.work_queue = this->work_queue_id;

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

media_stream_t sink_file::create_stream(presentation_clock_t& clock)
{
    stream_file_t stream(new stream_file(this->shared_from_this<sink_file>()));
    stream->register_sink(clock);

    return stream;
}

void sink_file::write_cb(void*)
{
    HRESULT hr = S_OK;
    if(this->video)
    {
        request_video_t request;
        {
            // pop the next item and queue a new work item if the queue is not empty
            request_video_t request2;
            scoped_lock lock(this->requests_mutex);
            if(!this->requests_video.pop(request))
                return;
            this->requests--;
            if(this->requests_video.get(request2))
                CHECK_HR(hr = 
                    this->write_callback->mf_put_work_item(this->shared_from_this<sink_file>()));
        }

        /*if(request.rp.flags & FLAG_DISCONTINUITY)
            CHECK_HR(hr = this->file_output->writer->SendStreamTick(0, this->last_timestamp));*/

        // TODO: the write_cb shouldn't be invoked when the buffer is empty
        if(!request.sample_view.buffer)
            return;

        for(auto&& item : request.sample_view.buffer->samples)
        {
            LONGLONG timestamp;
            CHECK_HR(hr = item->GetSampleTime(&timestamp));

            // (software encoder returns frames slightly in wrong order,
            // but mediawriter seems to deal with it)
            if(timestamp <= this->last_timestamp && !request.sample_view.software)
            {
                std::cout << "timestamp error in sink_file::write_cb video" << std::endl;
                assert_(false);
            }
            this->last_timestamp = timestamp;

            this->file_output->write_sample(true, item);
        }
    }
    else
    {
        request_audio_t request;
        {
            // pop the next item and queue a new work item if the queue is not empty
            request_audio_t request2;
            scoped_lock lock(this->requests_mutex);
            if(!this->requests_audio.pop(request))
                return;
            this->requests--;
            if(this->requests_audio.get(request2))
                CHECK_HR(hr =
                    this->write_callback->mf_put_work_item(this->shared_from_this<sink_file>()));
        }

        // TODO: the write_cb shouldn't be invoked when the buffer is empty
        if(!request.sample_view.buffer)
            return;
        for(auto&& item : request.sample_view.buffer->samples)
        {
            LONGLONG timestamp;
            CHECK_HR(hr = item->GetSampleTime(&timestamp));
            if(timestamp <= this->last_timestamp)
            {
                std::cout << "timestamp error in sink_file::write_cb audio" << std::endl;
                assert_(false);
            }
            this->last_timestamp = timestamp;

            this->file_output->write_sample(false, item);
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
    // try to set the initial time for the output;
    // the output will modify the sample timestamps so that they start at 0
    this->sink->file_output->set_initial_time(t);
}

media_stream::result_t stream_file::request_sample(request_packet& rp, const media_stream*)
{
    return this->sink->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_file::process_sample(
    const media_sample& sample, request_packet& rp, const media_stream*)
{
    HRESULT hr = S_OK;
    if(this->sink->video)
    {
        const media_sample_h264* sample_h264 = reinterpret_cast<const media_sample_h264*>(&sample);
        assert_(dynamic_cast<const media_sample_h264*>(&sample));

        sink_file::request_video_t request;
        request.rp = rp;
        request.stream = this;
        request.sample_view = *sample_h264;

        // add the new sample to queue and drop oldest sample if the queue is full
        sink_file::scoped_lock lock(this->sink->requests_mutex);
        this->sink->requests_video.push(request);
        this->sink->requests++;
        while(this->sink->requests > sink_file::max_queue_depth)
        {
            sink_file::request_video_t request;
            if(!this->sink->requests_video.pop(request))
                break;
            this->sink->requests--;
            std::cout << "------video output sample dropped------" << std::endl;
        }

        // add to work queue if the max queue depth isn't exceeded
        if(this->sink->requests <= sink_file::max_queue_depth)
            CHECK_HR(hr = this->sink->write_callback->mf_put_work_item(this->sink));
    }
    else
    {
        const media_sample_aac* sample_aac = reinterpret_cast<const media_sample_aac*>(&sample);
        assert_(dynamic_cast<const media_sample_aac*>(&sample));

        sink_file::request_audio_t request;
        request.rp = rp;
        request.stream = this;
        request.sample_view = *sample_aac;

        // add the new sample to queue and drop oldest sample if the queue is full
        sink_file::scoped_lock lock(this->sink->requests_mutex);
        this->sink->requests_audio.push(request);
        this->sink->requests++;
        while(this->sink->requests > sink_file::max_queue_depth)
        {
            sink_file::request_audio_t request;
            if(!this->sink->requests_audio.pop(request))
                break;
            this->sink->requests--;
            std::cout << "------audio output sample dropped------" << std::endl;
        }

        // add to work queue if the max queue depth isn't exceeded
        if(this->sink->requests <= sink_file::max_queue_depth)
            CHECK_HR(hr = this->sink->write_callback->mf_put_work_item(this->sink));
    }

    this->sink->session->give_sample(this, sample, rp, false);

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);

    return OK;
}