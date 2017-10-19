#include "source_displaycapture5.h"
#include <iostream>
#include <atomic>
#include <Mferror.h>

#pragma comment(lib, "D3D11.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

source_displaycapture5::source_displaycapture5(const media_session_t& sessio) : 
    media_source(session),
    newest_sample(new media_sample_texture)
{
}

media_stream_t source_displaycapture5::create_stream()
{
    return media_stream_t(
        new stream_displaycapture5(this->shared_from_this<source_displaycapture5>()));
}

HRESULT source_displaycapture5::initialize(
    UINT output_index, const CComPtr<ID3D11Device>& d3d11dev, const CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr;
    CComPtr<IDXGIDevice> dxgidev;
    CComPtr<IDXGIAdapter> dxgiadapter;
    CComPtr<IDXGIOutput> output;
    CComPtr<IDXGIOutput1> output1;

    this->d3d11dev = d3d11dev;
    this->d3d11devctx = devctx;

    // get dxgi dev
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&dxgidev));

    // get dxgi adapter
    CHECK_HR(hr = dxgidev->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiadapter));

    // get the primary output
    CHECK_HR(hr = dxgiadapter->EnumOutputs(output_index, &output));
    DXGI_OUTPUT_DESC desc;
    CHECK_HR(hr = output->GetDesc(&desc));

    // qi for output1
    CHECK_HR(hr = output->QueryInterface(&output1));

    // create the desktop duplication
    this->output_duplication = NULL;
    CHECK_HR(hr = output1->DuplicateOutput(this->d3d11dev, &this->output_duplication));

done:
    if(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
    {
        std::cerr << "maximum number of desktop duplication api applications running" << std::endl;
        return hr;
    }
    else if(FAILED(hr))
        throw std::exception();

    return hr;
}

// display capture mutex needed because for some reason acquirenextframe and
// releaseframe can deadlock with each other
std::recursive_mutex displaycapture_mutex;

bool source_displaycapture5::capture_frame(media_sample_texture_t& sample)
{
    scoped_lock lock(displaycapture_mutex);

    CComPtr<IDXGIResource> frame;
    CComPtr<ID3D11Texture2D> screen_frame;
    DXGI_OUTDUPL_FRAME_INFO frame_info = {0};
    HRESULT hr = S_OK;

    CHECK_HR(hr = this->output_duplication->AcquireNextFrame(0, &frame_info, &frame));
    CHECK_HR(hr = frame->QueryInterface(&screen_frame));

    UINT64 key = 1;
    if(!sample->texture)
    {
        /*CComPtr<IDXGIKeyedMutex> mutex;*/
        D3D11_TEXTURE2D_DESC screen_frame_desc;

        screen_frame->GetDesc(&screen_frame_desc);
        screen_frame_desc.MiscFlags = 0;
        screen_frame_desc.Usage = D3D11_USAGE_DEFAULT;
        /*screen_frame_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;*/
        // allow multithreaded read and write for shaders
        /*screen_frame_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;*/
        CHECK_HR(hr = this->d3d11dev->CreateTexture2D(&screen_frame_desc, NULL, &sample->texture));

        /*CHECK_HR(hr = sample->texture->QueryInterface(&mutex));*/
        key = 0;
    }

    // copy
    // TODO: do not copy if the frame hasnt changed
    /*if(frame_info.LastPresentTime.QuadPart != 0)*/
    {
        /*CComPtr<IDXGIKeyedMutex> frame_mutex;*/

        /*CHECK_HR(hr = sample->texture->QueryInterface(&frame_mutex));*/
        /*CHECK_HR(hr = frame_mutex->AcquireSync(key, INFINITE));*/

        this->d3d11devctx->CopyResource(sample->texture, screen_frame);
        key = 1;
        /*CHECK_HR(hr = frame_mutex->ReleaseSync(key));*/

        // update the newest sample
        std::atomic_exchange(&this->newest_sample, sample);
    }

done:
    this->output_duplication->ReleaseFrame();

    if(hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        /*std::cout << "FRAME IS NULL------------------" << std::endl;*/
        return false;
    }
    else if(FAILED(hr))
        throw std::exception();

    return true;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_displaycapture5::stream_displaycapture5(const source_displaycapture5_t& source) : 
    source(source),
    sample(new media_sample_texture)
{
    this->capture_frame_callback.Attach(new async_callback_t(&stream_displaycapture5::capture_frame_cb));
}

void stream_displaycapture5::capture_frame_cb(void*)
{
    // there exists a possibility for dead lock if another thread tries to read
    // the cached texture, and this thread has already locked the sample;
    // unlocking the capture frame mutex must be ensured before trying to lock the
    // cache texture
    media_sample_view_t sample_view(
        new media_sample_view(this->sample, media_sample_view::view_lock_t::LOCK_SAMPLE));
    bool frame_captured;
    source_displaycapture5::request_t request;

    // pull a request and capture a frame for it
    {
        scoped_lock lock(this->source->capture_frame_mutex);

        {
            scoped_lock lock(this->source->requests_mutex);
            request = this->source->requests.front();
            this->source->requests.pop();
        }

        frame_captured = this->source->capture_frame(this->sample);
    }

    if(!frame_captured)
    {
        // sample view must be reset to null before assigning a new sample view,
        // that is because the media_sample_view would lock the sample before
        // sample_view releasing its own reference to another sample_view
        sample_view.reset();
        sample_view.reset(
            new media_sample_view(std::atomic_load(&this->source->newest_sample), 
            media_sample_view::view_lock_t::READ_LOCK_SAMPLE));
    }
    else
        this->sample->unlock_write_lock_sample();

    request.first->process_sample(sample_view, request.second, NULL);
}

media_stream::result_t stream_displaycapture5::request_sample(request_packet& rp, const media_stream*)
{
    {
        scoped_lock lock(this->source->requests_mutex);
        this->source->requests.push(std::make_pair(this, rp));
    }

    // dispatch the capture request
    const HRESULT hr = this->capture_frame_callback->mf_put_work_item(
        this->shared_from_this<stream_displaycapture5>(),
        MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
    else if(hr == MF_E_SHUTDOWN)
        return FATAL_ERROR;

    return OK;

    //if(this->source->capture_frame(this->sample))
    //{
    //    media_sample_view_t sample_view(new media_sample_view(this->sample));
    //    return this->process_sample(sample_view, rp);
    //}
    //else
    //{
    //    // TODO: is source call could also be dispatched similarly like is sink call
    //    media_sample_view_t sample_view(new media_sample_view(this->null_sample));
    //    return this->process_sample(sample_view, rp);
    //}
}

media_stream::result_t stream_displaycapture5::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    // TODO: do not use request time because the actual timestamp
    // might be greatly different
    sample_view->get_sample()->timestamp = rp.request_time;
    this->source->session->give_sample(this, sample_view, rp, true);
    return OK;
}