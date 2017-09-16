#pragma once
#include "media_source.h"
#include "media_stream.h"
#include "media_sample.h"
#include "presentation_clock.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <memory>

#pragma comment(lib, "dxgi")

class source_displaycapture3 : public media_source
{
    friend class stream_preview;
    friend class stream_displaycapture3;
private:
    CComPtr<IDXGIOutputDuplication> output_duplication;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<ID3D11DeviceContext> d3d11devctx;
    CComPtr<ID3D11Texture2D> screen_frame;
    HANDLE screen_frame_handle;
public:
    explicit source_displaycapture3(const media_session_t& session);

    media_stream_t create_stream();
    HRESULT initialize(CComPtr<ID3D11Device>&, CComPtr<ID3D11DeviceContext>&);
    HANDLE capture_frame();
};

typedef std::shared_ptr<source_displaycapture3> source_displaycapture3_t;

class stream_displaycapture3 : public media_stream
{
private:
    AsyncCallback<stream_displaycapture3> callback;
    source_displaycapture3_t source;

    HRESULT capture_cb(IMFAsyncResult*);
public:
    stream_displaycapture3(const source_displaycapture3_t& source);
    bool get_clock(presentation_clock_t& c) {return this->source->session->get_current_clock(c);}

    // called by media session
    result_t request_sample();
    // called by source_displaycapture
    result_t process_sample(const media_sample_t&);
};