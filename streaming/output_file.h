#pragma once

#include "media_sample.h"
#include <mfidl.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <atlbase.h>
#include <memory>
#include <mutex>

class output_file
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    volatile bool stopped;
    std::mutex stop_mutex;
    time_unit initial_time;

    HANDLE stopped_signal;
    CComPtr<IMFMediaType> video_type;
    CComPtr<IMFMediaType> audio_type;
    CComPtr<IMFMediaSink> mpeg_media_sink;
    CComPtr<IMFSinkWriter> writer;
    CComPtr<IMFByteStream> byte_stream;
public:
    output_file();
    ~output_file();

    void initialize(
        bool null_file,
        HANDLE stopped_signal,
        const CComPtr<IMFMediaType>& video_type,
        const CComPtr<IMFMediaType>& audio_type);

    // the initial time can be only set once, subsequent calls have no effect
    void set_initial_time(time_unit);

    // write_sample modifies the sample's timestamp
    void write_sample(bool video, const CComPtr<IMFSample>& sample);
    void force_stop();
};

typedef std::shared_ptr<output_file> output_file_t;