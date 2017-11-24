#include "transform_videoprocessor.h"
#include "assert.h"
#include <mfapi.h>
#include <Mferror.h>
#include <algorithm>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

transform_videoprocessor::transform_videoprocessor(
    const media_session_t& session, std::recursive_mutex& context_mutex) :
    media_source(session), context_mutex(context_mutex)
{
}

void transform_videoprocessor::initialize(UINT input_streams,
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
    CHECK_HR(hr = this->enumerator->GetVideoProcessorCaps(&this->videoprocessor_caps));

    if(input_streams > this->max_input_streams())
        CHECK_HR(hr = E_FAIL);

    // create the video processor
    CHECK_HR(hr = this->videodevice->CreateVideoProcessor(this->enumerator, 0, &this->videoprocessor));

done:
    if(FAILED(hr))
        throw std::exception();
}

stream_videoprocessor_t transform_videoprocessor::create_stream()
{
    return stream_videoprocessor_t(
        new stream_videoprocessor(this->shared_from_this<transform_videoprocessor>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_videoprocessor::stream_videoprocessor(const transform_videoprocessor_t& transform) :
    transform(transform),
    output_buffer(new media_buffer_texture),
    output_buffer_null(new media_buffer_texture),
    view_initialized(false),
    samples_received(0)
{
    this->processing_callback.Attach(new async_callback_t(&stream_videoprocessor::processing_cb));
}

void stream_videoprocessor::processing_cb(void*)
{
    // TODO: any kind of parameter changing should only happen by reinitializing
    // the streams;
    // actually, some parameters can be changed by setting the packet number point
    // where new parameters will apply

    HRESULT hr = S_OK;
    bool blit = false;
    {
        // lock the output sample
        media_sample_view_t sample_view;
        time_unit timestamp = std::numeric_limits<time_unit>::max();

        UINT i = 0, j = 0;
        // construct a list of streams that will be blit onto output surface;
        // this must be used because for some reason atiumdva has a null pointer read violation
        // if setting enabled state of a stream to false
        // TODO: this container can be listed in class declaration so that it won't be
        // allocated every time
        std::vector<D3D11_VIDEO_PROCESSOR_STREAM> streams;
        streams.reserve(this->input_streams.size());
        for(auto it = this->input_streams.begin(); it != this->input_streams.end(); it++, i++)
        {
            CComPtr<ID3D11VideoProcessorInputView> input_view;
            CComPtr<ID3D11Texture2D> texture = it->first.sample_view->texture_buffer->texture;
            const media_sample_view_videoprocessor::params_t& params = it->first.sample_view->params;
            D3D11_VIDEO_PROCESSOR_STREAM& native_params = this->input_streams_params[i];

            // create the input view for the texture
            if(texture)
            {
                D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc;
                desc.FourCC = 0; // uses the same format the input resource has
                desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipSlice = 0;
                desc.Texture2D.ArraySlice = 0;
                CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorInputView(
                    texture, this->transform->enumerator, &desc, &input_view));
            }

            // set parameters for the stream
            native_params.Enable = (texture != NULL);
            native_params.OutputIndex = 0;
            native_params.InputFrameOrField = 0;
            native_params.PastFrames = 0;
            native_params.FutureFrames = 0;
            native_params.ppPastSurfaces = NULL;
            native_params.pInputSurface = input_view;
            native_params.ppFutureSurfaces = NULL;
            native_params.ppPastSurfacesRight = NULL;
            native_params.pInputSurfaceRight = NULL;
            native_params.ppFutureSurfacesRight = NULL;

            // increment the input_view ref count because it's referenced by the stream params
            if(input_view.p)
                input_view.p->AddRef();
            else
                continue;

            streams.push_back(native_params);
            blit = true;
            // use the earliest timestamp;
            // actually, max must be used so that the timestamp stays incremental
            // (using max only applies to displaycapture where the buffers are shared between streams)
            timestamp = std::min(timestamp, it->first.sample_view->sample.timestamp);

            scoped_lock lock(this->transform->context_mutex);
            // set the source rectangle for the stream
            // (the part of the stream texture which will be included in the blit)
            this->transform->videocontext->VideoProcessorSetStreamSourceRect(
                this->transform->videoprocessor, j, TRUE, &params.source_rect);

            // set the destination rectangle for the stream
            // (where the stream will appear in the output blit)
            this->transform->videocontext->VideoProcessorSetStreamDestRect(
                this->transform->videoprocessor, j, TRUE, &params.dest_rect);

            j++;
        }

        if(blit)
        {
            scoped_lock lock(this->transform->context_mutex);
            // set the target rectangle for the output
            // (sets the rectangle where the output blit on the output texture will appear)
            this->transform->videocontext->VideoProcessorSetOutputTargetRect(
                this->transform->videoprocessor, TRUE, &this->output_target_rect);

            // lock the output buffer before blitting
            sample_view.reset(new media_sample_view(this->output_buffer));
            // dxva 2 and direct3d11 video seems to be similar
            // https://msdn.microsoft.com/en-us/library/windows/desktop/cc307964(v=vs.85).aspx#Video_Process_Blit
            // the video processor alpha blends the input streams to the target output
            CHECK_HR(hr = this->transform->videocontext->VideoProcessorBlt(
                this->transform->videoprocessor, this->output_view,
                0, streams.size(), &streams[0]));
        }
        else
            // TODO: read lock buffers is just a workaround for a deadlock bug
            sample_view.reset(new media_sample_view(this->output_buffer_null, media_sample_view::READ_LOCK_BUFFERS));

        //if(texture && texture2)
        //{
        //    sample_view.reset(new media_sample_view(this->output_buffer));

        //    // create the input view for the sample to be converted
        //    CComPtr<ID3D11VideoProcessorInputView> input_view[2];

        //    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc;
        //    desc.FourCC = 0; // uses the same format the input resource has
        //    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        //    desc.Texture2D.MipSlice = 0;
        //    desc.Texture2D.ArraySlice = 0;
        //    CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorInputView(
        //        texture, this->transform->enumerator, &desc, &input_view[0]));
        //    CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorInputView(
        //        texture2, this->transform->enumerator, &desc, &input_view[1]));

        //    // TODO: do not create input views every time

        //    // convert
        //    D3D11_VIDEO_PROCESSOR_STREAM stream[2];
        //    RECT source_rect;
        //    source_rect.top = source_rect.left = 0;
        //    source_rect.right = 1920;
        //    source_rect.bottom = 1080;

        //    for(int i = 0; i < 2; i++)
        //    {
        //        stream[i].Enable = TRUE;
        //        stream[i].OutputIndex = 0;
        //        stream[i].InputFrameOrField = 0;
        //        stream[i].PastFrames = 0;
        //        stream[i].FutureFrames = 0;
        //        stream[i].ppPastSurfaces = NULL;
        //        stream[i].pInputSurface = input_view[i];
        //        stream[i].ppFutureSurfaces = NULL;
        //        stream[i].ppPastSurfacesRight = NULL;
        //        stream[i].pInputSurfaceRight = NULL;
        //        stream[i].ppFutureSurfacesRight = NULL;
        //    }

        //    scoped_lock lock(this->transform->context_mutex);

        //    // set the target rectangle for the output
        //    // (sets the rectangle where the output blit on the output texture will appear)
        //    this->transform->videocontext->VideoProcessorSetOutputTargetRect(
        //        this->transform->videoprocessor, TRUE, &source_rect);

        //    // set the source rectangle of the streams
        //    // (the part of the stream texture which will be included in the blit)
        //    this->transform->videocontext->VideoProcessorSetStreamSourceRect(
        //        this->transform->videoprocessor, 0, TRUE, &source_rect);
        //    this->transform->videocontext->VideoProcessorSetStreamSourceRect(
        //        this->transform->videoprocessor, 1, TRUE, &source_rect);

        //    // set the destination rectangle of the streams
        //    // (where the stream will appear in the output blit)
        //    this->transform->videocontext->VideoProcessorSetStreamDestRect(
        //        this->transform->videoprocessor, 0, TRUE, &source_rect);
        //    RECT rect;
        //    rect.top = rect.left = 0;
        //    rect.right = 1920 / 3;
        //    rect.bottom = 1080 / 3;
        //    this->transform->videocontext->VideoProcessorSetStreamDestRect(
        //        this->transform->videoprocessor, 1, TRUE, &rect);

        //    // dxva 2 and direct3d11 video seems to be similar
        //    // https://msdn.microsoft.com/en-us/library/windows/desktop/cc307964(v=vs.85).aspx#Video_Process_Blit
        //    // the video processor alpha blends the input streams to the target output
        //    const UINT stream_count = 2;
        //    CHECK_HR(hr = this->transform->videocontext->VideoProcessorBlt(
        //        this->transform->videoprocessor, this->output_view,
        //        0, stream_count, stream));
        //}
        //else
        //    // TODO: read lock buffers is just a workaround for a deadlock bug
        //    sample_view.reset(new media_sample_view(this->output_buffer_null, media_sample_view::READ_LOCK_BUFFERS));

        //// use the earliest timestamp;
        //// actually, max must be used so that the timestamp stays incremental
        //sample_view->sample.timestamp =
        //    std::min(
        //    this->pending_request2.sample_view->sample.timestamp, 
        //    this->pending_request.sample_view->sample.timestamp);

        // set the timestamp
        sample_view->sample.timestamp = timestamp;
        // rps are equivalent in every input stream
        request_packet rp = this->input_streams[0].first.rp;

        // reset the sample view from the input stream packets so it is unlocked;
        // reset the rps so that there aren't any circular dependencies at shutdown
        for(auto it = this->input_streams.begin(); it != this->input_streams.end(); it++)
        {
            it->first.sample_view = NULL;
            it->first.rp = request_packet();
        }

        this->transform->session->give_sample(this, sample_view, rp, false);
    }

done:
    if(blit)
        for(auto it = this->input_streams_params.begin(); it != this->input_streams_params.end(); it++)
            if(it->pInputSurface)
            {
                it->pInputSurface->Release();
                it->pInputSurface = NULL;
            };

    if(FAILED(hr))
        throw std::exception();
}

void stream_videoprocessor::add_input_stream(const media_stream* stream)
{
    if(this->input_streams.size() >= this->transform->max_input_streams())
        throw std::exception();

    D3D11_VIDEO_PROCESSOR_STREAM native_params;
    // pinputsurface is initialized because it is referenced in processing_cb
    native_params.pInputSurface = NULL;

    this->input_streams.push_back(std::make_pair(packet(), stream));
    this->input_streams_params.push_back(native_params);
}

media_stream::result_t stream_videoprocessor::request_sample(
    request_packet& rp, const media_stream*)
{
    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_videoprocessor::process_sample(
    const media_sample_view_t& sample_view_, request_packet& rp, const media_stream* prev_stream)
{
    media_sample_view_videoprocessor_t sample_view = cast<media_sample_view_videoprocessor>(sample_view_);
    assert_(sample_view);
    CComPtr<ID3D11Texture2D> texture = sample_view->texture_buffer->texture;

    // this function needs to be locked because media session dispatches the process sample calls
    // to work queues in a same node
    scoped_lock lock(this->mutex);

    // create the output view if it hasn't been created
    if(!this->view_initialized && texture)
    {
        this->view_initialized = true;
        HRESULT hr = S_OK;

        // create output texture with same format as the sample
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        CHECK_HR(hr = this->transform->d3d11dev->CreateTexture2D(
            &desc, NULL, &this->output_buffer->texture));
        CHECK_HR(hr = this->output_buffer->texture->QueryInterface(&this->output_buffer->resource));

        // create output view
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc;
        view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
        CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorOutputView(
            this->output_buffer->texture, this->transform->enumerator,
            &view_desc, &this->output_view));

        // set the videoprocessor output target rect
        this->output_target_rect.left = 0;
        this->output_target_rect.top = 0;
        this->output_target_rect.right = desc.Width;
        this->output_target_rect.bottom = desc.Height;
    }

    {
        auto it = std::find_if(this->input_streams.begin(), this->input_streams.end(), 
            [prev_stream](const input_streams_t& e) {return e.second == prev_stream;});
        assert_(it != this->input_streams.end());

        it->first.rp = rp;
        it->first.sample_view = sample_view;
        this->samples_received++;

        assert_(this->samples_received <= (int)this->input_streams.size());
        if(this->samples_received == (int)this->input_streams.size())
        {
            this->processing_cb(NULL);
            this->samples_received = 0;
        }
    }

    return OK;
done:
    this->view_initialized = false;
    throw std::exception();
    return FATAL_ERROR;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_view_videoprocessor::media_sample_view_videoprocessor(
    const params_t& params, 
    const media_buffer_texture_t& texture_buffer, view_lock_t view_lock) :
    params(params),
    media_sample_view_texture(texture_buffer, view_lock)
{
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


void stream_videoprocessor_controller::get_params(params_t& params) const
{
    scoped_lock lock(this->mutex);
    params = this->params;
}

void stream_videoprocessor_controller::set_params(const params_t& params)
{
    scoped_lock lock(this->mutex);
    this->params = params;
}