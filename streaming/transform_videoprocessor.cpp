#include "transform_videoprocessor.h"
#include <mfapi.h>
#include <Mferror.h>
#include <algorithm>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#ifdef min
#undef min
#endif

transform_videoprocessor::transform_videoprocessor(const media_session_t& session) :
    media_source(session)
{
}

HRESULT transform_videoprocessor::initialize(const CComPtr<ID3D11Device>& d3d11dev)
{
    HRESULT hr = S_OK;

    this->d3d11dev = d3d11dev;
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&this->videodevice));
    
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

stream_videoprocessor_t transform_videoprocessor::create_stream(ID3D11DeviceContext* devctx)
{
    return stream_videoprocessor_t(
        new stream_videoprocessor(devctx, this->shared_from_this<transform_videoprocessor>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_videoprocessor::stream_videoprocessor(
    ID3D11DeviceContext* devctx,
    const transform_videoprocessor_t& transform) :
    transform(transform),
    output_sample(new media_sample_texture),
    null_sample(new media_sample_texture),
    view_initialized(false),
    primary_stream(NULL)
{
    this->processing_callback.Attach(new async_callback_t(&stream_videoprocessor::processing_cb));

    HRESULT hr = S_OK;
    CHECK_HR(hr = devctx->QueryInterface(&this->videocontext));

done:
    if(FAILED(hr))
        throw std::exception();
}

void stream_videoprocessor::processing_cb(void*)
{
    // TODO: any kind of parameter changing should only happen by reinitializing
    // the streams;
    // actually, some parameters can be changed by setting the packet number point
    // where new parameters will apply

    HRESULT hr = S_OK;
    {
        // lock the output sample
        media_sample_view_t sample_view;

        CComPtr<ID3D11Texture2D> texture = 
            this->pending_request2.sample_view->get_sample<media_sample_texture>()->texture;
        CComPtr<ID3D11Texture2D> texture2 =
            this->pending_request.sample_view->get_sample<media_sample_texture>()->texture;

        if(texture && texture2)
        {
            sample_view.reset(new media_sample_view(this->output_sample));

            // create the input view for the sample to be converted
            CComPtr<ID3D11VideoProcessorInputView> input_view[2];

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
            this->videocontext->VideoProcessorSetOutputTargetRect(
                this->transform->videoprocessor, TRUE, &source_rect);

            // set the source rectangle of the streams
            // (the part of the stream texture which will be included in the blit)
            this->videocontext->VideoProcessorSetStreamSourceRect(
                this->transform->videoprocessor, 0, TRUE, &source_rect);
            this->videocontext->VideoProcessorSetStreamSourceRect(
                this->transform->videoprocessor, 1, TRUE, &source_rect);

            // set the destination rectangle of the streams
            // (where the stream will appear in the output blit)
            this->videocontext->VideoProcessorSetStreamDestRect(
                this->transform->videoprocessor, 0, TRUE, &source_rect);
            RECT rect;
            rect.top = rect.left = 0;
            rect.right = 1920 / 3;
            rect.bottom = 1080 / 3;
            this->videocontext->VideoProcessorSetStreamDestRect(
                this->transform->videoprocessor, 1, TRUE, &rect);

            // dxva 2 and direct3d11 video seems to be similar
            // https://msdn.microsoft.com/en-us/library/windows/desktop/cc307964(v=vs.85).aspx#Video_Process_Blit
            // the video processor alpha blends the input streams to the target output
            const UINT stream_count = 2;
            CHECK_HR(hr = this->videocontext->VideoProcessorBlt(
                this->transform->videoprocessor, this->output_view,
                0, stream_count, stream));
        }
        else
            sample_view.reset(new media_sample_view(this->null_sample));

        // use the earliest timestamp
        this->output_sample->timestamp = 
            std::min(
            this->pending_request2.sample_view->get_sample()->timestamp, 
            this->pending_request.sample_view->get_sample()->timestamp);

        request_packet rp = this->pending_request.rp;

        // reset the sample view from the pending packet so it is unlocked
        this->pending_request.sample_view = NULL;
        this->pending_request2.sample_view = NULL;
        // reset the rps so that there aren't any circular dependencies at shutdown
        this->pending_request.rp = request_packet();
        this->pending_request2.rp = request_packet();

        this->transform->session->give_sample(this, sample_view, rp, false);
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

media_stream::result_t stream_videoprocessor::request_sample(
    request_packet& rp, const media_stream* prev_stream)
{
    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_videoprocessor::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream* prev_stream)
{
    CComPtr<ID3D11Texture2D> texture = sample_view->get_sample<media_sample_texture>()->texture;

    // this function needs to be locked because media session dispatches the process sample calls
    // to work queues in a same node
    std::unique_lock<std::recursive_mutex> lock(this->mutex);

    // create the output view if it hasn't been created
    if(!this->view_initialized && texture)
    {
        this->view_initialized = true;
        HRESULT hr = S_OK;

        // create output texture with same format as the sample
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        desc.MiscFlags = 0;
        /*desc.BindFlags = D3D11_BIND_RENDER_TARGET;*/
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        CHECK_HR(hr = this->transform->d3d11dev->CreateTexture2D(
            &desc, NULL, &this->output_sample->texture));
        CHECK_HR(hr = this->output_sample->texture->QueryInterface(&this->output_sample->resource));
        CHECK_HR(hr = this->output_sample->resource->GetSharedHandle(&this->output_sample->shared_handle));

        // create output view
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc;
        view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
        CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorOutputView(
            this->output_sample->texture, this->transform->enumerator,
            &view_desc, &this->output_view));
    }

    if(prev_stream == this->primary_stream)
    {
        this->pending_request.rp = rp;
        this->pending_request.sample_view = sample_view;
    }
    else
    {
        this->pending_request2.rp = rp;
        this->pending_request2.sample_view = sample_view;
    }

    if(this->pending_request.sample_view && this->pending_request2.sample_view)
    {
        lock.unlock();
        this->processing_cb(NULL);
    }

    return OK;
done:
    this->view_initialized = false;
    throw std::exception();
    return FATAL_ERROR;
}