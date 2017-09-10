#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "media_sample.h"
#include "presentation_clock.h"
#include <mutex>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#pragma comment(lib, "dxgi")

/*

allocate displaycapture device
allocate media session
allocate topology
allocate source_live
start source_live(allocates streams)
add source_live to topology
add topology to media session
start media session (media session calls media sink with time)

*/

// must be managed by shared_ptr
class source_displaycapture : public media_source
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    CComPtr<IDXGIOutputDuplication> output_duplication;
    std::mutex mutex;
    LARGE_INTEGER start_time;
public:
    explicit source_displaycapture(const media_session_t& session);

    media_stream_t create_stream(presentation_clock_t& clock);

    HRESULT initialize(ID3D11Device*);
    // frame capturing is synchronized;
    // may return NULL
    media_sample_t capture_frame(UINT timeout, LARGE_INTEGER& device_time_stamp);

    //// the source must be started beforehand
    //stream_live* get_stream() const;

    // change_size(2d size)

    /*bool initialize();*/
};

typedef std::shared_ptr<source_displaycapture> source_displaycapture_t;

// stream_displaycapture doesn't need scheduling capabilities
class stream_displaycapture : public media_stream, public presentation_clock_sink
{
private:
    AsyncCallback<stream_displaycapture> callback;
    source_displaycapture_t source;
    LARGE_INTEGER device_start_time;
    time_unit start_time;

    bool on_clock_start(time_unit);
    void on_clock_stop(time_unit);
    void scheduled_callback(time_unit due_time);
    HRESULT capture_cb(IMFAsyncResult*);
public:
    stream_displaycapture(const source_displaycapture_t& source, presentation_clock_t& clock);

    bool get_clock(presentation_clock_t&);

    // called by media session
    result_t request_sample();
    // called by source_displaycapture
    result_t process_sample(const media_sample_t&);
};