#pragma once
#include "media_sink.h"
#include "media_stream.h"
#include <Windows.h>

class stream_preview;

class sink_preview : public media_sink
{
public:
    explicit sink_preview(const media_session_t& session);

    media_stream_t create_stream(presentation_clock_t& clock);

    // (presentation clock can be accessed from media session)
    // set_presentation_clock

    // initializes the window
    void initialize(HWND);

    //// begin requesting samples
    //bool start(media_stream&);
};

typedef std::shared_ptr<sink_preview> sink_preview_t;

class stream_preview : public media_stream, public presentation_clock_sink
{
private:
    sink_preview_t sink;

    bool on_clock_start(time_unit);
    void on_clock_stop(time_unit);
    void scheduled_callback(time_unit due_time);
public:
    stream_preview(const sink_preview_t& sink, presentation_clock_t& clock);

    presentation_clock_t get_clock();

    // called by sink_preview
    result_t request_sample();
    // called by media session
    result_t process_sample(const media_sample_t&);

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() {return IUnknownImpl::AddRef();}
    ULONG STDMETHODCALLTYPE Release() {return IUnknownImpl::Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) {return E_NOTIMPL;}
};