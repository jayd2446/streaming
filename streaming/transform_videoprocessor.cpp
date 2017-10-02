#include "transform_videoprocessor.h"
#include <mfapi.h>
#include <Mferror.h>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

transform_videoprocessor::transform_videoprocessor(const media_session_t& session) :
    media_source(session), view_initialized(false)
{
}

HRESULT transform_videoprocessor::initialize(
    const CComPtr<ID3D11Device>& d3d11dev, ID3D11DeviceContext* devctx)
{
    HRESULT hr = S_OK;

    UINT support;
    hr = d3d11dev->CheckFormatSupport(DXGI_FORMAT_NV12, &support);
    if(support & D3D11_FORMAT_SUPPORT_TEXTURE2D)
    {
        hr = S_OK;
    }
    if(support & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT)
    {
        hr = S_OK;
    }
    if(support & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT)
    {
        hr = S_OK;
    }
    if(support & D3D11_FORMAT_SUPPORT_VIDEO_ENCODER)
    {
        hr = S_OK;
    }
    if(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)
    {
        hr = S_OK;
    }

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
    media_stream_t temp;
    temp.Attach(new stream_videoprocessor(
        std::dynamic_pointer_cast<transform_videoprocessor>(this->shared_from_this())));
    return temp;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_videoprocessor::stream_videoprocessor(const transform_videoprocessor_t& transform) :
    transform(transform),
    processing_callback(this, &stream_videoprocessor::processing_cb),
    output_sample(new media_sample)
{
}

HRESULT stream_videoprocessor::processing_cb(IMFAsyncResult*)
{
    HRESULT hr = S_OK;
    {
        // lock the processor so that only one processing task is performed at a time
        scoped_lock lock(this->transform->videoprocessor_mutex);

        // create the input view for the sample to be converted
        CComPtr<ID3D11VideoProcessorInputView> input_view;
        CComPtr<ID3D11Texture2D> texture/*, texture2*/;
        CComPtr<IDXGIKeyedMutex> mutex;
        CComPtr<IDXGISurface> surface;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc;

        desc.FourCC = 0; // uses the same format the input resource has
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        desc.Texture2D.ArraySlice = 0;
        CHECK_HR(hr = this->transform->d3d11dev->OpenSharedResource(
            this->pending_sample->frame, __uuidof(ID3D11Texture2D), (void**)&texture));

        /*D3D11_TEXTURE2D_DESC texdesc;
        texture->GetDesc(&texdesc);
        texdesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        texdesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        texdesc.Usage = D3D11_USAGE_DEFAULT;
        texdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;*/

        /*CHECK_HR(hr = this->transform->d3d11dev->CreateTexture2D(
            &texdesc, NULL, &texture2));*/

        CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorInputView(
            texture, this->transform->enumerator, &desc, &input_view));

        // convert
        D3D11_VIDEO_PROCESSOR_STREAM stream;
        RECT source_rect;
        source_rect.top = source_rect.left = 0;
        source_rect.right = 1920;
        source_rect.bottom = 1080;

        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.PastFrames = 0;
        stream.FutureFrames = 0;
        stream.ppPastSurfaces = NULL;
        stream.pInputSurface = input_view;
        stream.ppFutureSurfaces = NULL;
        stream.ppPastSurfacesRight = NULL;
        stream.pInputSurfaceRight = NULL;
        stream.ppFutureSurfacesRight = NULL;

        /*this->transform->videocontext->VideoProcessorSetStreamFrameFormat(
            this->transform->videoprocessor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        this->transform->videocontext->VideoProcessorSetStreamSourceRect(
            this->transform->videoprocessor, 0, TRUE, &source_rect);*/

        // because the input texture uses shared keyed mutex the texture must be locked
        // before operating it
        CHECK_HR(hr = texture->QueryInterface(&surface));
        CHECK_HR(hr = surface->QueryInterface(&mutex));
        CHECK_HR(hr = mutex->AcquireSync(1, INFINITE));

        // dxva 2 and direct3d11 video seems to be similar
        // https://msdn.microsoft.com/en-us/library/windows/desktop/cc307964(v=vs.85).aspx#Video_Process_Blit
        // the video processor alpha blends the input streams to the target output
        CHECK_HR(hr = this->transform->videocontext->VideoProcessorBlt(
            this->transform->videoprocessor, this->transform->output_view,
            0, 1, &stream));

        CHECK_HR(hr = mutex->ReleaseSync(1));

        this->output_sample->frame = (HANDLE)this->transform->output_texture.p;
        this->output_sample->timestamp = this->pending_sample->timestamp;
        this->output_sample->mutex.lock();
        this->transform->session->give_sample(this, this->output_sample, this->request_time, false);
    }

    return S_OK;

done:
    if(FAILED(hr))
        throw std::exception();

    return S_OK;
}

media_stream::result_t stream_videoprocessor::request_sample(time_unit request_time)
{
    if(!this->transform->session->request_sample(this, request_time, false))
        return FATAL_ERROR;
    return OK;
}

extern bool bb;

media_stream::result_t stream_videoprocessor::process_sample(
    const media_sample_t& sample, time_unit request_time)
{
    // pass the null sample to downstream
    if(!sample->frame)
    {
        if(!this->transform->session->give_sample(this, sample, request_time, false))
            return FATAL_ERROR;
        return OK;
    }

    bb = false;

    // TODO: resources shouldn't be initialized here because of the multithreaded
    // nature they might be initialized more than once

    // create the output view if it hasn't been created
    if(!this->transform->view_initialized)
    {
        this->transform->view_initialized = true;

        HRESULT hr = S_OK;
        CComPtr<ID3D11Texture2D> texture, texture2;
        CHECK_HR(hr = this->transform->d3d11dev->OpenSharedResource(
            sample->frame, __uuidof(ID3D11Texture2D), (void**)&texture));

        {
            scoped_lock lock(this->transform->videoprocessor_mutex);
            // create output texture with nv12 color format
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);
            desc.MiscFlags = 0;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.Format = DXGI_FORMAT_NV12;
            CHECK_HR(hr = this->transform->d3d11dev->CreateTexture2D(
                &desc, NULL, &this->transform->output_texture));

            // create output view
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc;
            view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            view_desc.Texture2D.MipSlice = 0;
            CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorOutputView(
                this->transform->output_texture, this->transform->enumerator,
                &view_desc, &this->transform->output_view));
        }
    }

    this->request_time = request_time;
    {
        scoped_lock lock(this->transform->videoprocessor_mutex);
        this->pending_sample = sample;

        const HRESULT hr = MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 
            &this->processing_callback, NULL);
        if(FAILED(hr) && hr != MF_E_SHUTDOWN)
            throw std::exception();
        else if(hr == MF_E_SHUTDOWN)
            return FATAL_ERROR;
    }

    return OK;

done:
    this->transform->view_initialized = false;
    throw std::exception();
    return FATAL_ERROR;
}