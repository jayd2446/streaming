#include "sink_mpeg.h"
#include <iostream>
#include <Mferror.h>

#define LAG_BEHIND 1000000/*(FPS60_INTERVAL * 6)*/

sink_mpeg::sink_mpeg(const media_session_t& session) : media_sink(session)
{
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
    // TODO: process the sample

    return OK;
}