#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "media_sample.h"
#include <memory>
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
class source_displaycapture : 
    public media_source, 
    public std::enable_shared_from_this<source_displaycapture>
{
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
private:
    CComPtr<IDXGIOutputDuplication> output_duplication;
    std::mutex mutex;
    LARGE_INTEGER start_time;
public:
    explicit source_displaycapture(const media_session_t& session);

    media_stream_t create_stream();

    HRESULT initialize(ID3D11Device*);
    // frame capturing is synchronized;
    // may return NULL
    media_sample_t capture_frame(UINT timeout);

    //// the source must be started beforehand
    //stream_live* get_stream() const;

    // change_size(2d size)

    /*bool initialize();*/
};

typedef std::shared_ptr<source_displaycapture> source_displaycapture_t;

class stream_displaycapture : public media_stream
{
private:
    AsyncCallback<stream_displaycapture> callback;
    source_displaycapture_t source;

    HRESULT capture_cb(IMFAsyncResult*);
public:
    explicit stream_displaycapture(const source_displaycapture_t& source);

    // called by media session
    result_t request_sample();
    // called by source_displaycapture
    result_t process_sample(const media_sample_t&);
};