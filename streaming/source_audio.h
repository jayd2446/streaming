#pragma once
#include "media_source.h"
#include "media_stream.h"
#include <memory>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <queue>
#include <mutex>
#include <utility>

#pragma comment(lib, "Mfplat.lib")

class source_audio : public media_source
{
    friend class stream_audio;
public:
    class source_reader_callback;
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef std::pair<LONGLONG /*timestamp in 100 nanosecond units*/, CComPtr<IMFSample>> sample_t;
private:
    CComPtr<IMFMediaSource> media_source;
    CComPtr<IMFSourceReaderCallback> source_reader_cb;
    CComPtr<IMFSourceReader> source_reader;
    CComPtr<IMFMediaType> source_media_type;

    std::recursive_mutex samples_mutex;
    std::queue<sample_t> samples;

    HRESULT create_source_reader(const PROPVARIANT&);
public:
    explicit source_audio(const media_session_t& session);
    ~source_audio();

    HRESULT initialize();
};

typedef std::shared_ptr<source_audio> source_audio_t;

class stream_audio : public media_stream
{
private:
    source_audio_t source;
public:
    explicit stream_audio(const source_audio_t& source);
};