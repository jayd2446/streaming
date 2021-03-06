#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include "media_sample.h"
#include "request_packet.h"
#include "request_queue_handler.h"
#include "output_class.h"
#include <memory>
#include <limits>

// TODO: rename to sink_output or similar

template<class SinkFile>
class stream_file;

template<class Request>
class sink_file final : public media_component, request_queue_handler<Request>
{
    friend class stream_file<sink_file>;
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef typename request_queue_handler<Request>::request_t request_t;
    typedef typename request_queue_handler<Request>::request_queue request_queue;
private:
    output_class_t output;
    LONGLONG last_timestamp;
    bool video;

    // request_queue_handler
    bool on_serve(typename request_queue::request_t&) override;
    typename request_queue::request_t* next_request() override;
public:
    explicit sink_file(const media_session_t& session);

    void initialize(const output_class_t& output, bool video);
    media_stream_t create_stream(media_message_generator_t&&);
};

typedef sink_file<media_component_h264_video_args_t> sink_file_video;
typedef std::shared_ptr<sink_file_video> sink_output_video_t;

typedef sink_file<media_component_aac_audio_args_t> sink_file_audio;
typedef std::shared_ptr<sink_file_audio> sink_output_audio_t;

template<class SinkFile>
class stream_file final : public media_stream_message_listener
{
    friend typename SinkFile;
public:
    typedef SinkFile sink_file;
    typedef std::shared_ptr<sink_file> sink_file_t;
    typedef media_stream::result_t result_t;
private:
    sink_file_t sink;
public:
    explicit stream_file(const sink_file_t&);

    result_t request_sample(const request_packet&, const media_stream*) override;
    result_t process_sample(
        const media_component_args*, const request_packet&, const media_stream*) override;
};

typedef stream_file<sink_file_video> stream_file_video;
typedef std::shared_ptr<stream_file_video> stream_file_video_t;

typedef stream_file<sink_file_audio> stream_file_audio;
typedef std::shared_ptr<stream_file_audio> stream_file_audio_t;


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

#undef min
#undef max

template<typename T>
sink_file<T>::sink_file(const media_session_t& session) : 
    media_component(session),
    last_timestamp(std::numeric_limits<LONGLONG>::min()),
    video(false)
{
}

template<typename T>
void sink_file<T>::initialize(const output_class_t& output, bool video)
{
    this->output = output;
    this->video = video;
}

template<typename T>
media_stream_t sink_file<T>::create_stream(media_message_generator_t&& message_generator)
{
    typedef stream_file<sink_file> stream_file;
    typedef std::shared_ptr<stream_file> stream_file_t;

    stream_file_t stream(new stream_file(this->shared_from_this<sink_file>()));
    stream->register_listener(message_generator);

    return stream;
}

template<typename T>
bool sink_file<T>::on_serve(typename request_queue::request_t& request)
{
    if(request.sample)
    {
        for(const auto& frame : request.sample->sample->frames)
        {
            const LONGLONG timestamp = (LONGLONG)frame.ts;
            const LONGLONG dur = (LONGLONG)frame.dur;

            // TODO: print if frames in wrong order

            this->last_timestamp = timestamp;
            this->output->write_sample(this->video, frame.sample);
        }

        // currently it is assumed that the sink file is connected directly to the video_sink
        this->session->give_sample(request.stream, &(*request.sample), request.rp);
    }

    return true;
}

template<typename T>
typename sink_file<T>::request_queue::request_t* sink_file<T>::next_request()
{
    return this->requests.get();
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<typename T>
stream_file<T>::stream_file(const sink_file_t& sink) : 
    sink(sink),
    media_stream_message_listener(sink.get())
{
}

template<typename T>
media_stream::result_t stream_file<T>::request_sample(const request_packet& rp, const media_stream*)
{
    this->sink->requests.initialize_queue(rp);
    return this->sink->session->request_sample(this, rp) ? OK : FATAL_ERROR;
}

template<typename T>
media_stream::result_t stream_file<T>::process_sample(
    const media_component_args* args_, const request_packet& rp, const media_stream*)
{
    typename sink_file::request_queue::request_t request;
    request.rp = rp;
    request.stream = this;
    if(args_)
    {
        typedef typename sink_file::request_t::value_type request_t;
        const request_t& args = static_cast<const request_t&>(*args_);
        request.sample = std::make_optional(args);
    }
    this->sink->requests.push(request);

    // pass null requests downstream
    if(!args_)
        this->sink->session->give_sample(this, NULL, request.rp);

    this->sink->serve();

    return OK;
}

#undef CHECK_HR