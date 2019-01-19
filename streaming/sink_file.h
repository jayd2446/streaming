#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include "media_sample.h"
#include "request_packet.h"
#include "output_file.h"
#include <memory>
#include <mutex>

class sink_file : public media_sink
{
    friend class stream_file;
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef request_queue<media_component_h264_video_args_t> request_queue_video;
    typedef request_queue<media_component_aac_audio_args_t> request_queue_audio;
    typedef request_queue_video::request_t request_video_t;
    typedef request_queue_audio::request_t request_audio_t;
    typedef async_callback<sink_file> async_callback_t;
private:
    output_file_t file_output;
    request_queue_video requests_video;
    request_queue_audio requests_audio;
    std::mutex requests_mutex, process_mutex;
    /*DWORD work_queue_id;*/
    bool video;
    int requests;

    LONGLONG last_timestamp;

    /*CComPtr<async_callback_t> write_callback;*/
    void process();
public:
    explicit sink_file(const media_session_t& session);
    ~sink_file();

    void initialize(const output_file_t& file_output, bool video);
    media_stream_t create_stream(presentation_clock_t&&);
};

typedef std::shared_ptr<sink_file> sink_file_t;

class stream_file : public media_stream_clock_sink
{
    friend class sink_file;
private:
    sink_file_t sink;

    void on_component_start(time_unit);
public:
    explicit stream_file(const sink_file_t&);

    result_t request_sample(const request_packet&, const media_stream*);
    result_t process_sample(const media_component_args*, const request_packet&, const media_stream*);
};

typedef std::shared_ptr<stream_file> stream_file_t;