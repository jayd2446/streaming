#pragma once
#include "media_sample.h"
#include "IUnknownImpl.h"
#include "AsyncCallback.h"
#include <memory>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <atlbase.h>

/*

live_source should implement this behaviour and request for a new frame from
dxgi whenever it gets a request sample event;
this will stall the pipeline, but frame copying would be required for the pipeline
not to stall.

*/

#pragma comment(lib, "dxgi")

#define DISPLAYCAPTURE_RATE_MS 16 // ~60 fps

// TODO: scheduling must be more accurate
// TODO: handle device lost
class device_displaycapture : std::enable_shared_from_this<device_displaycapture>
{
    class capture_callback;
private:
    capture_callback* callback;
    media_sample_t sample;
    CComPtr<IDXGIOutputDuplication> output_duplication;

    int refs;
public:
    // TODO: throws
    explicit device_displaycapture();
    ~device_displaycapture();

    // must call shutdown if initializing failed
    HRESULT initialize(ID3D11Device*);
    void shutdown();

    media_sample_t get_sample() const {return this->sample;}
};

typedef std::shared_ptr<device_displaycapture> device_displaycapture_t;