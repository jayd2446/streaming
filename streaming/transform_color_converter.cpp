#include "transform_color_converter.h"
#include "transform_h264_encoder.h"
#include <iostream>
#include <mfapi.h>
#include <Mferror.h>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
//void CHECK_HR(HRESULT hr)
//{
//    if(FAILED(hr))
//        throw HR_EXCEPTION(hr);
//}


transform_color_converter::transform_color_converter(
    const media_session_t& session, context_mutex_t context_mutex) :
    media_source(session),
    texture_pool(new buffer_pool),
    context_mutex(context_mutex)
{
}

transform_color_converter::~transform_color_converter()
{
    // dispose the pool so that the cyclic dependency between the wrapped container and its
    // elements is broken
    buffer_pool::scoped_lock lock(this->texture_pool->mutex);
    this->texture_pool->dispose();
}

HRESULT transform_color_converter::initialize(
    const control_class_t& ctrl_pipeline,
    const CComPtr<ID3D11Device>& d3d11dev, ID3D11DeviceContext* devctx)
{
    HRESULT hr = S_OK;

    this->ctrl_pipeline = ctrl_pipeline;
    this->d3d11dev = d3d11dev;
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&this->videodevice));
    CHECK_HR(hr = devctx->QueryInterface(&this->videocontext));
    
    // check the supported capabilities of the video processor
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc;
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator = transform_h264_encoder::frame_rate_num;
    desc.InputFrameRate.Denominator = transform_h264_encoder::frame_rate_den;
    desc.InputWidth = transform_h264_encoder::frame_width;
    desc.InputHeight = transform_h264_encoder::frame_height;
    desc.OutputFrameRate.Numerator = transform_h264_encoder::frame_rate_num;
    desc.OutputFrameRate.Denominator = transform_h264_encoder::frame_rate_den;
    desc.OutputWidth = transform_h264_encoder::frame_width;
    desc.OutputHeight = transform_h264_encoder::frame_height;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    CHECK_HR(hr = this->videodevice->CreateVideoProcessorEnumerator(&desc, &this->enumerator));
    UINT flags;
    // https://msdn.microsoft.com/en-us/library/windows/desktop/mt427455(v=vs.85).aspx
    // b8g8r8a8 and nv12 must be supported by direct3d 11 devices
    // as video processor input and output;
    // it must be also supported by texture2d for render target
    CHECK_HR(hr = this->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &flags));
    if(!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
        throw HR_EXCEPTION(hr);
    CHECK_HR(hr = this->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags));
    if(!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        throw HR_EXCEPTION(hr);

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);

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
    transform(transform)
{
    HRESULT hr = S_OK;

    CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessor(
        this->transform->enumerator, 0, &this->videoprocessor));

    // Auto stream processing(the default) can hurt power consumption
    {
        scoped_lock lock(*this->transform->context_mutex);
        this->transform->videocontext->VideoProcessorSetStreamAutoProcessingMode(
            this->videoprocessor, 0, FALSE);
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void stream_color_converter::initialize_buffer(const media_buffer_texture_t& buffer)
{
    HRESULT hr = S_OK;

    // create output texture with nv12 color format
    // nv12 format is 1 byte
    // TODO: figure out the real size of nv12 format
    static BYTE init_data[transform_h264_encoder::frame_width * transform_h264_encoder::frame_height *
    sizeof(DWORD)]
        = {0};

    D3D11_SUBRESOURCE_DATA subrsrc;
    subrsrc.pSysMem = init_data;
    subrsrc.SysMemPitch = transform_h264_encoder::frame_width;
    subrsrc.SysMemSlicePitch = 0;

    D3D11_TEXTURE2D_DESC desc;
    desc.Width = transform_h264_encoder::frame_width;
    desc.Height = transform_h264_encoder::frame_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.Format = DXGI_FORMAT_NV12;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CHECK_HR(hr = this->transform->d3d11dev->CreateTexture2D(&desc, &subrsrc, &buffer->texture));

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void stream_color_converter::processing_cb(void*)
{
    HRESULT hr = S_OK;

    // get output buffer from the pool or create a new one
    media_buffer_texture_t output_buffer;
    {
        transform_color_converter::buffer_pool::scoped_lock lock(this->transform->texture_pool->mutex);
        if(this->transform->texture_pool->container.empty())
        {
            // create new buffer
            media_buffer_pooled_texture_t pooled_buffer(new media_buffer_pooled_texture(
                this->transform->texture_pool));
            output_buffer = pooled_buffer->create_pooled_buffer();
            this->initialize_buffer(output_buffer);
            /*std::cout << "creating new..." << std::endl;*/
        }
        else
        {
            output_buffer = this->transform->texture_pool->container.top()->create_pooled_buffer();
            this->transform->texture_pool->container.pop();
            /*std::cout << "reusing..." << std::endl;*/
        }
    }

    media_sample_texture sample;
    CComPtr<ID3D11Texture2D> texture = this->pending_packet.sample_view.buffer->texture;
    assert_(texture);

    // create the output view
    CComPtr<ID3D11VideoProcessorOutputView> output_view;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc;
    view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipSlice = 0;
    // TODO: include this outputview in the pool aswell
    CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorOutputView(
        output_buffer->texture, this->transform->enumerator, &view_desc, &output_view));

    // scope is important here so that the context mutex is unlocked;
    // failure to unlock it before proceeding to next component
    // introduces a deadlock scenario
    {
        // create the input view for the sample to be converted
        CComPtr<ID3D11VideoProcessorInputView> input_view;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc;
        desc.FourCC = 0; // uses the same format the input resource has
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        desc.Texture2D.ArraySlice = 0;
        CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorInputView(
            texture, this->transform->enumerator, &desc, &input_view));

        // convert
        D3D11_VIDEO_PROCESSOR_STREAM stream;
        RECT dst_rect;
        dst_rect.top = dst_rect.left = 0;
        dst_rect.right = transform_h264_encoder::frame_width;
        dst_rect.bottom = transform_h264_encoder::frame_height;

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

        scoped_lock lock(*this->transform->context_mutex);

        // set the target rectangle for the output
        // (sets the rectangle where the output blit on the output texture will appear)
        this->transform->videocontext->VideoProcessorSetOutputTargetRect(
            this->videoprocessor, TRUE, &dst_rect);

        // set the source rectangle of the stream
        // (the part of the stream texture which will be included in the blit);
        // false indicates that the whole source is read
        this->transform->videocontext->VideoProcessorSetStreamSourceRect(
            this->videoprocessor, 0, FALSE, &dst_rect);

        // set the destination rectangle of the stream
        // (where the stream will appear in the output blit)
        this->transform->videocontext->VideoProcessorSetStreamDestRect(
            this->videoprocessor, 0, TRUE, &dst_rect);

        // blit
        const UINT stream_count = 1;
        CHECK_HR(hr = this->transform->videocontext->VideoProcessorBlt(
            this->videoprocessor, output_view,
            0, stream_count, &stream));
    }

done:
    if(FAILED(hr))
    {
        PRINT_ERROR(hr);
        this->transform->request_reinitialization(this->transform->ctrl_pipeline);
    }

    sample.buffer = output_buffer;
    sample.timestamp = this->pending_packet.sample_view.timestamp;
        
    request_packet rp = this->pending_packet.rp;

    // reset the sample view from the pending packet so it is unlocked
    this->pending_packet.sample_view.buffer = NULL;
    // remove the rps so that there aren't any circular dependencies at shutdown
    this->pending_packet.rp = request_packet();

    // give the sample to downstream
    this->transform->session->give_sample(this, sample, rp, false);

//done:
//    if(FAILED(hr))
//        throw HR_EXCEPTION(hr);
}

media_stream::result_t stream_color_converter::request_sample(request_packet& rp, const media_stream*)
{
    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_color_converter::process_sample(
    const media_sample& sample_view_, request_packet& rp, const media_stream*)
{
    const media_sample_texture& sample_view = 
        reinterpret_cast<const media_sample_texture&>(sample_view_);

    /*CComPtr<ID3D11Texture2D> texture = sample_view->get_buffer<media_buffer_texture>()->texture;*/

    this->pending_packet.rp = rp;
    this->pending_packet.sample_view = sample_view;

    this->processing_cb(NULL);
    return OK;
}