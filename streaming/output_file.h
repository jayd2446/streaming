#pragma once

#include "media_sample.h"
#include "wtl.h"
#include <mfidl.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <atlbase.h>
#include <memory>
#include <mutex>

#define RECORDING_STOPPED_MESSAGE (WM_APP + 1)

class output_file
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    volatile bool stopped;
    std::mutex stop_mutex;

    ATL::CWindow recording_initiator;
    CComPtr<IMFMediaType> video_type;
    CComPtr<IMFMediaType> audio_type;
    CComPtr<IMFMediaSink> mpeg_media_sink;
    CComPtr<IMFByteStream> byte_stream;
public:
    CComPtr<IMFSinkWriter> writer;

    output_file();
    ~output_file();

    void initialize(
        bool null_file,
        ATL::CWindow recording_initiator,
        const CComPtr<IMFMediaType>& video_type,
        const CComPtr<IMFMediaType>& audio_type);

    // write_sample modifies the sample's timestamp
    void write_sample(bool video, const CComPtr<IMFSample>& sample);
    void force_stop();
};

typedef std::shared_ptr<output_file> output_file_t;