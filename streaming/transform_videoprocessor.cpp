#include "transform_videoprocessor.h"
#include <mfapi.h>
#include <Mferror.h>
#include <algorithm>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#ifdef min
#undef min
#endif

transform_videoprocessor::transform_videoprocessor(const media_session_t& session) :
    media_source(session), view_initialized(false),
    output_texture_handle(NULL)
{
}

HRESULT transform_videoprocessor::initialize(
    const CComPtr<ID3D11Device>& d3d11dev, ID3D11DeviceContext* devctx)
{
    HRESULT hr = S_OK;

    this->d3d11dev = d3d11dev;
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&this->videodevice));
    CHECK_HR(hr = devctx->QueryInterface(&this->videocontext));
    
    // check the supported capabilities of the video processor
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc;
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator = 60;
    desc.InputFrameRate.Denominator = 1;
    desc.InputWidth = 1920;
    desc.InputHeight = 1080;
    desc.OutputFrameRate.Numerator = 60;
    desc.OutputFrameRate.Denominator = 1;
    desc.OutputWidth = 1920;
    desc.OutputHeight = 1080;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    CHECK_HR(hr = this->videodevice->CreateVideoProcessorEnumerator(&desc, &this->enumerator));
    UINT flags;
    // https://msdn.microsoft.com/en-us/library/windows/desktop/mt427455(v=vs.85).aspx
    // b8g8r8a8 and nv12 must be supported by direct3d 11 devices
    // as video processor input and output;
    // it must be also supported by texture2d for render target
    CHECK_HR(hr = this->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &flags));
    if(!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
        throw std::exception();
    CHECK_HR(hr = this->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags));
    if(!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        throw std::exception();

    // create the video processor
    CHECK_HR(hr = this->videodevice->CreateVideoProcessor(this->enumerator, 0, &this->videoprocessor));

    // set the state for the video processor


done:
    if(FAILED(hr))
        throw std::exception();

    return hr;
}

media_stream_t transform_videoprocessor::create_stream()
{
    return media_stream_t(
        new stream_videoprocessor(this->shared_from_this<transform_videoprocessor>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_videoprocessor::stream_videoprocessor(const transform_videoprocessor_t& transform) :
    transform(transform),
    output_sample(new media_sample)
{
    this->processing_callback.Attach(new async_callback_t(&stream_videoprocessor::processing_cb));
}

void stream_videoprocessor::processing_cb()
{
    HRESULT hr = S_OK;
    {
        // TODO: video processing must be locked so that the context isnt
        // modified in multiple threads

        // lock the processor so that only one processing task is performed at a time
        scoped_lock lock(this->transform->videoprocessor_mutex);

        HANDLE frame, frame2;

        this->transform->requests_mutex.lock();
        transform_videoprocessor::packet p = this->transform->requests_2.front();
        this->transform->requests_2.pop();
        auto it = this->transform->requests.find(p.rp.request_time);
        transform_videoprocessor::packet p2 = it->second;
        this->transform->requests.erase(it);
        this->transform->requests_mutex.unlock();

        frame = p2.sample->frame;
        frame2 = p.sample->frame;

        assert(frame != frame2 || !frame || !frame2);
        if(frame && frame2)
        {
            // create the input view for the sample to be converted
            CComPtr<ID3D11VideoProcessorInputView> input_view[2];
            CComPtr<ID3D11Texture2D> texture, texture2;
            CComPtr<IDXGIKeyedMutex> mutex, mutex2, mutex3;
            CComPtr<IDXGISurface> surface, surface2, surface3;

            CHECK_HR(hr = this->transform->d3d11dev->OpenSharedResource(
                frame, __uuidof(ID3D11Texture2D), (void**)&texture));
            CHECK_HR(hr = this->transform->d3d11dev->OpenSharedResource(
                frame2, __uuidof(ID3D11Texture2D), (void**)&texture2));

            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc;
            desc.FourCC = 0; // uses the same format the input resource has
            desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipSlice = 0;
            desc.Texture2D.ArraySlice = 0;
            CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorInputView(
                texture, this->transform->enumerator, &desc, &input_view[0]));
            CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorInputView(
                texture2, this->transform->enumerator, &desc, &input_view[1]));

            // convert
            D3D11_VIDEO_PROCESSOR_STREAM stream[2];
            RECT source_rect;
            source_rect.top = source_rect.left = 0;
            source_rect.right = 1920;
            source_rect.bottom = 1080;

            for(int i = 0; i < 2; i++)
            {
                stream[i].Enable = TRUE;
                stream[i].OutputIndex = 0;
                stream[i].InputFrameOrField = 0;
                stream[i].PastFrames = 0;
                stream[i].FutureFrames = 0;
                stream[i].ppPastSurfaces = NULL;
                stream[i].pInputSurface = input_view[i];
                stream[i].ppFutureSurfaces = NULL;
                stream[i].ppPastSurfacesRight = NULL;
                stream[i].pInputSurfaceRight = NULL;
                stream[i].ppFutureSurfacesRight = NULL;
            }

            // set the target rectangle for the output
            // (sets the rectangle where the output blit on the output texture will appear)
            this->transform->videocontext->VideoProcessorSetOutputTargetRect(
                this->transform->videoprocessor, TRUE, &source_rect);

            // set the source rectangle of the streams
            // (the part of the stream texture which will be included in the blit)
            this->transform->videocontext->VideoProcessorSetStreamSourceRect(
                this->transform->videoprocessor, 0, TRUE, &source_rect);
            this->transform->videocontext->VideoProcessorSetStreamSourceRect(
                this->transform->videoprocessor, 1, TRUE, &source_rect);

            // set the destination rectangle of the streams
            // (where the stream will appear in the output blit)
            this->transform->videocontext->VideoProcessorSetStreamDestRect(
                this->transform->videoprocessor, 0, TRUE, &source_rect);
            RECT rect;
            rect.top = rect.left = 0;
            rect.right = 1920 / 3;
            rect.bottom = 1080 / 3;
            this->transform->videocontext->VideoProcessorSetStreamDestRect(
                this->transform->videoprocessor, 1, TRUE, &rect);

            // because the input texture uses shared keyed mutex the texture must be locked
            // before operating it
            CHECK_HR(hr = texture->QueryInterface(&surface));
            CHECK_HR(hr = texture2->QueryInterface(&surface2));
            CHECK_HR(hr = this->transform->output_texture->QueryInterface(&surface3));
            CHECK_HR(hr = surface->QueryInterface(&mutex));
            CHECK_HR(hr = surface2->QueryInterface(&mutex2));
            CHECK_HR(hr = surface3->QueryInterface(&mutex3));
            CHECK_HR(hr = mutex->AcquireSync(1, INFINITE));
            CHECK_HR(hr = mutex2->AcquireSync(1, INFINITE));
            CHECK_HR(hr = mutex3->AcquireSync(1, INFINITE));

            // dxva 2 and direct3d11 video seems to be similar
            // https://msdn.microsoft.com/en-us/library/windows/desktop/cc307964(v=vs.85).aspx#Video_Process_Blit
            // the video processor alpha blends the input streams to the target output
            const UINT stream_count = 2;
            CHECK_HR(hr = this->transform->videocontext->VideoProcessorBlt(
                this->transform->videoprocessor, this->transform->output_view,
                0, stream_count, stream));

            CHECK_HR(hr = mutex3->ReleaseSync(1));
            CHECK_HR(hr = mutex2->ReleaseSync(1));
            CHECK_HR(hr = mutex->ReleaseSync(1));
        }

        //// TODO: only the sink should drop request packets,
        //// since packets should not be dropped inside the pipeline

        // lock the output sample
        this->output_sample->lock_sample();
        this->output_sample->frame = this->transform->output_texture_handle;
        // use the earliest timestamp
        this->output_sample->timestamp = 
            std::min(p2.sample->timestamp, p.sample->timestamp);

        // unlock the frames
        p2.sample->unlock_sample();
        p.sample->unlock_sample();

        this->transform->session->give_sample(this, this->output_sample, p.rp, false);
    }

    return;
done:
    if(FAILED(hr))
        throw std::exception();
}

media_stream::result_t stream_videoprocessor::request_sample(request_packet& rp)
{
    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}
media_stream::result_t stream_videoprocessor::process_sample(
    const media_sample_t& sample, request_packet& rp)
{
    // TODO: resources shouldn't be initialized here because of the multithreaded
    // nature they might be initialized more than once

    // create the output view if it hasn't been created
    if(!this->transform->view_initialized && sample->frame)
    {
        this->transform->view_initialized = true;

        HRESULT hr = S_OK;
        CComPtr<ID3D11Texture2D> texture;
        CHECK_HR(hr = this->transform->d3d11dev->OpenSharedResource(
            sample->frame, __uuidof(ID3D11Texture2D), (void**)&texture));

        {
            scoped_lock lock(this->transform->videoprocessor_mutex);
            CComPtr<IDXGIResource> idxgiresource;
            CComPtr<IDXGIKeyedMutex> mutex;
            // create output texture with nv12 color format
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            /*desc.BindFlags = D3D11_BIND_RENDER_TARGET;*/
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            CHECK_HR(hr = this->transform->d3d11dev->CreateTexture2D(
                &desc, NULL, &this->transform->output_texture));
            CHECK_HR(hr = this->transform->output_texture->QueryInterface(&idxgiresource));
            CHECK_HR(hr = idxgiresource->GetSharedHandle(&this->transform->output_texture_handle));
            CHECK_HR(hr = this->transform->output_texture->QueryInterface(&mutex));
            CHECK_HR(hr = mutex->AcquireSync(0, INFINITE));
            CHECK_HR(hr = mutex->ReleaseSync(1));

            // create output view
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc;
            view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            view_desc.Texture2D.MipSlice = 0;
            CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorOutputView(
                this->transform->output_texture, this->transform->enumerator,
                &view_desc, &this->transform->output_view));
        }
    }

    {
        scoped_lock lock(this->transform->requests_mutex);

        auto it = this->transform->requests.find(rp.request_time);
        if(it != this->transform->requests.end())
        {
            transform_videoprocessor::packet p = {rp, sample};
            this->transform->requests_2.push(p);

            const HRESULT hr = this->processing_callback->mf_put_work_item(
                this->shared_from_this<stream_videoprocessor>(),
                MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
            if(FAILED(hr) && hr != MF_E_SHUTDOWN)
                throw std::exception();
            else if(hr == MF_E_SHUTDOWN)
                return FATAL_ERROR;
        }
        else
        {
            this->transform->requests[rp.request_time].rp = rp;
            this->transform->requests[rp.request_time].sample = sample;
        }
    }

    return OK;
done:
    this->transform->view_initialized = false;
    throw std::exception();
    return FATAL_ERROR;
}