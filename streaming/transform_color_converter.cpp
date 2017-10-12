#include "transform_color_converter.h"
#include <mfapi.h>
#include <Mferror.h>

//#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
void CHECK_HR(HRESULT hr)
{
    if(FAILED(hr))
        throw std::exception();
}

transform_color_converter::transform_color_converter(const media_session_t& session) :
    media_source(session), output_texture_handle(NULL), view_initialized(false)
{
}

HRESULT transform_color_converter::initialize(
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

media_stream_t transform_color_converter::create_stream()
{
    return media_stream_t(
        new stream_color_converter(this->shared_from_this<transform_color_converter>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_color_converter::stream_color_converter(const transform_color_converter_t& transform) :
    transform(transform),
    output_sample(new media_sample)
{
    this->processing_callback.Attach(new async_callback_t(&stream_color_converter::processing_cb));
}

void stream_color_converter::processing_cb(void*)
{
    HRESULT hr = S_OK;
    {
        scoped_lock lock(this->transform->videoprocessor_mutex);

        this->transform->requests_mutex.lock();
        transform_color_converter::packet p = this->transform->requests.front();
        this->transform->requests.pop();
        this->transform->requests_mutex.unlock();

        // lock the output sample
        this->output_sample->lock_sample();

        HANDLE frame = p.sample->frame;
        if(frame)
        {
            // create the input view for the sample to be converted
            CComPtr<ID3D11VideoProcessorInputView> input_view;
            CComPtr<ID3D11Texture2D> texture;
            CComPtr<IDXGIKeyedMutex> mutex, mutex2;
            CComPtr<IDXGISurface> surface, surface2;

            CHECK_HR(hr = this->transform->d3d11dev->OpenSharedResource(
                frame, __uuidof(ID3D11Texture2D), (void**)&texture));

            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc;
            desc.FourCC = 0; // uses the same format the input resource has
            desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipSlice = 0;
            desc.Texture2D.ArraySlice = 0;
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

            // set the target rectangle for the output
            // (sets the rectangle where the output blit on the output texture will appear)
            this->transform->videocontext->VideoProcessorSetOutputTargetRect(
                this->transform->videoprocessor, TRUE, &source_rect);

            // set the source rectangle of the stream
            // (the part of the stream texture which will be included in the blit)
            this->transform->videocontext->VideoProcessorSetStreamSourceRect(
                this->transform->videoprocessor, 0, TRUE, &source_rect);

            // set the destination rectangle of the stream
            // (where the stream will appear in the output blit)
            this->transform->videocontext->VideoProcessorSetStreamDestRect(
                this->transform->videoprocessor, 0, TRUE, &source_rect);

            // because the input texture uses shared keyed mutex the texture must be locked
            // before operating it
            CHECK_HR(hr = texture->QueryInterface(&surface));
            CHECK_HR(hr = this->transform->output_texture->QueryInterface(&surface2));
            CHECK_HR(hr = surface->QueryInterface(&mutex));
            CHECK_HR(hr = surface2->QueryInterface(&mutex2));
            CHECK_HR(hr = mutex->AcquireSync(1, INFINITE));
            CHECK_HR(hr = mutex2->AcquireSync(1, INFINITE));

            const UINT stream_count = 1;
            CHECK_HR(hr = this->transform->videocontext->VideoProcessorBlt(
                this->transform->videoprocessor, this->transform->output_view,
                0, stream_count, &stream));

            CHECK_HR(hr = mutex2->ReleaseSync(1));
            CHECK_HR(hr = mutex->ReleaseSync(1));
        }

        this->output_sample->frame = this->transform->output_texture_handle;
        this->output_sample->timestamp = p.sample->timestamp;

        // unlock the input sample
        p.sample->unlock_sample();

        this->transform->session->give_sample(this, this->output_sample, p.rp, false);
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

media_stream::result_t stream_color_converter::request_sample(request_packet& rp)
{
    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_color_converter::process_sample(
    const media_sample_t& sample, request_packet& rp)
{
    // TODO: resources shouldn't be initialized here because of the multithreaded
    // nature they might be initialized more than once

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
            desc.Format = DXGI_FORMAT_NV12;
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

        transform_color_converter::packet p = {rp, sample};
        this->transform->requests.push(p);

        const HRESULT hr = this->processing_callback->mf_put_work_item(
            this->shared_from_this<stream_color_converter>(),
            MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
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