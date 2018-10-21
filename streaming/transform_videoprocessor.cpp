#include "transform_videoprocessor.h"
#include "transform_h264_encoder.h"
#include "assert.h"
#include <mfapi.h>
#include <Mferror.h>
#include <algorithm>
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
//void CHECK_HR(HRESULT hr)
//{
//    if(FAILED(hr))
//        throw HR_EXCEPTION(hr);
//}
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

transform_videoprocessor::transform_videoprocessor(
    const media_session_t& session, context_mutex_t context_mutex) :
    media_source(session), context_mutex(context_mutex)
{
}

void transform_videoprocessor::initialize(
    const control_class_t& ctrl_pipeline,
    const CComPtr<ID3D11Device>& d3d11dev, 
    const CComPtr<ID3D11DeviceContext>& devctx)
{
    HRESULT hr = S_OK;

    this->ctrl_pipeline = ctrl_pipeline;
    this->d3d11dev = d3d11dev;
    this->devctx = devctx;
    CHECK_HR(hr = this->d3d11dev->QueryInterface(&this->videodevice));
    CHECK_HR(hr = this->devctx->QueryInterface(&this->videocontext));
    
    // check the supported capabilities of the video processor
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc;
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator = transform_h264_encoder::frame_rate_num;
    desc.InputFrameRate.Denominator = transform_h264_encoder::frame_rate_den;
    desc.InputWidth = canvas_width;
    desc.InputHeight = canvas_height;
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
    CHECK_HR(hr = this->enumerator->GetVideoProcessorCaps(&this->videoprocessor_caps));
    if(!(this->videoprocessor_caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ALPHA_STREAM))
        throw HR_EXCEPTION(hr);
    // amd doesn't support alpha fill, which means that VideoProcessorSetOutputAlphaFillMode
    // is unavailable
    /*if(!(this->videoprocessor_caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ALPHA_FILL))
        throw HR_EXCEPTION(hr);*/
    if(this->videoprocessor_caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_LEGACY)
        throw HR_EXCEPTION(hr);

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

stream_videoprocessor_t transform_videoprocessor::create_stream()
{
    return stream_videoprocessor_t(
        new stream_videoprocessor(this->shared_from_this<transform_videoprocessor>()));
}

UINT transform_videoprocessor::max_input_streams() const
{
    // maxinputstreams and maxstreamstates are 0x10 for amd devices,
    // but actually only support up to 10 simultaneous streams

    return std::min(10U, std::min(this->videoprocessor_caps.MaxInputStreams, 
            this->videoprocessor_caps.MaxStreamStates));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_videoprocessor::stream_videoprocessor(const transform_videoprocessor_t& transform) :
    transform(transform),
    samples_received(0)
{
    HRESULT hr = S_OK;

    this->output_buffer[0].reset(new media_buffer_texture);
    this->output_buffer[1].reset(new media_buffer_texture);

    this->streams.reserve(this->transform->max_input_streams());

    // create the videoprocessor;
    // videoprocessor state information is per stream
    CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessor(
        this->transform->enumerator, 0, &this->videoprocessor));

    {
        scoped_lock lock(*this->transform->context_mutex);
        D3D11_VIDEO_COLOR color;
        color.RGBA.A = 1.f;
        color.RGBA.B = 0.f;
        color.RGBA.G = 0.f;
        color.RGBA.R = 0.f;
        this->transform->videocontext->VideoProcessorSetOutputBackgroundColor(
            this->videoprocessor, FALSE, &color);

        // Auto stream processing(the default) can hurt power consumption
        for(UINT i = 0; i < this->transform->max_input_streams(); i++)
            this->transform->videocontext->VideoProcessorSetStreamAutoProcessingMode(
                this->videoprocessor, i, FALSE);
    }

    // create the output and intermediate texture buffers
    // create output texture
    {
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
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        CHECK_HR(hr = this->transform->d3d11dev->CreateTexture2D(
            &desc, NULL, &this->output_buffer[0]->texture));
        /*CHECK_HR(hr = this->output_buffer[0]->texture->QueryInterface(&this->output_buffer[0]->resource));*/

        CHECK_HR(hr = this->transform->d3d11dev->CreateTexture2D(
            &desc, NULL, &this->output_buffer[1]->texture));
        /*CHECK_HR(hr = this->output_buffer[1]->texture->QueryInterface(&this->output_buffer[1]->resource));*/

        // create output view
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc;
        view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
        CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorOutputView(
            this->output_buffer[0]->texture, this->transform->enumerator,
            &view_desc, &this->output_view[0]));

        CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorOutputView(
            this->output_buffer[1]->texture, this->transform->enumerator,
            &view_desc, &this->output_view[1]));

        // set the videoprocessor output target rect
        this->output_target_rect.left = 0;
        this->output_target_rect.top = 0;
        this->output_target_rect.right = desc.Width;
        this->output_target_rect.bottom = desc.Height;
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void stream_videoprocessor::release_input_streams(std::vector<D3D11_VIDEO_PROCESSOR_STREAM>& streams)
{
    for(auto it = streams.begin(); it != streams.end(); it++)
        it->pInputSurface->Release();
    streams.clear();
}

HRESULT stream_videoprocessor::set_input_stream(
    const media_sample_videoprocessor::params_t& stream_params,
    const media_sample_videoprocessor::params_t& user_params,
    const CComPtr<ID3D11Texture2D>& texture, D3D11_VIDEO_PROCESSOR_STREAM& native_params, UINT j,
    bool& ret)
{
    HRESULT hr = S_OK;
    ret = false;

    if(!texture)
        return hr;

    RECT src_rect, dst_rect;
    const bool visible = this->calculate_stream_rects(stream_params, user_params, src_rect, dst_rect);

    if(!visible)
        return hr;

    // create the input view for the texture
    CComPtr<ID3D11VideoProcessorInputView> input_view;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc;
    desc.FourCC = 0; // uses the same format the input resource has
    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.ArraySlice = 0;
    CHECK_HR(hr = this->transform->videodevice->CreateVideoProcessorInputView(
        texture, this->transform->enumerator, &desc, &input_view));

    // bind input view to the stream
    native_params.Enable = TRUE;
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
    native_params.pInputSurface->AddRef();

    // set parameters for the stream
    {
        scoped_lock lock(*this->transform->context_mutex);
        // set the source rectangle for the stream
        // (the part of the stream texture which will be included in the blit)
        this->transform->videocontext->VideoProcessorSetStreamSourceRect(
            this->videoprocessor, j, TRUE, &src_rect);

        // set the destination rectangle for the stream
        // (where the stream will appear in the output blit)
        this->transform->videocontext->VideoProcessorSetStreamDestRect(
            this->videoprocessor, j, TRUE, &dst_rect);

        // planar alpha for each stream need to be set so that the bitblt will
        // alpha blend all the streams
        this->transform->videocontext->VideoProcessorSetStreamAlpha(
            this->videoprocessor, j, stream_params.enable_alpha, 1.f);
    }

    ret = true;
done:
    return hr;
}

HRESULT stream_videoprocessor::blit(
    const std::vector<D3D11_VIDEO_PROCESSOR_STREAM>& streams,
    const CComPtr<ID3D11VideoProcessorOutputView>& output_view)
{
    scoped_lock lock(*this->transform->context_mutex);
    // set the target rectangle for the output
    // (sets the rectangle where the output blit on the output texture will appear)
    this->transform->videocontext->VideoProcessorSetOutputTargetRect(
        this->videoprocessor, TRUE, &this->output_target_rect);

    if(!streams.empty())
    {
        /*if(streams.size() == 1)
            this->transform->videocontext->VideoProcessorSetStreamAlpha(
                this->videoprocessor, 0, TRUE, 1.f);*/

        // the output texture is assumed to be locked
        // dxva 2 and direct3d11 video seems to be similar
        // https://msdn.microsoft.com/en-us/library/windows/desktop/cc307964(v=vs.85).aspx#Video_Process_Blit
        // the video processor alpha blends the input streams to the target output
        return this->transform->videocontext->VideoProcessorBlt(
            this->videoprocessor, output_view,
            0, (UINT)streams.size(), &streams[0]);
    }
    else
    {
        CComPtr<ID3D11Resource> rsrc;
        CComPtr<ID3D11Texture2D> tex;
        CComPtr<ID3D11RenderTargetView> render_target_view;
        HRESULT hr = S_OK;

        output_view->GetResource(&rsrc);
        CHECK_HR(hr = rsrc->QueryInterface(&tex));
        CHECK_HR(hr = 
            this->transform->d3d11dev->CreateRenderTargetView(tex, NULL, &render_target_view));
        static FLOAT black_rgba[4] = {0.f, 0.f, 0.f, 1.f};
        this->transform->devctx->ClearRenderTargetView(render_target_view, black_rgba);

    done:
        return hr;
    }
}

bool stream_videoprocessor::calculate_stream_rects(
    const media_sample_videoprocessor::params_t& stream_params, 
    const media_sample_videoprocessor::params_t& user_params,
    RECT& src_rect, RECT& dst_rect)
{
    // user src rect and sample dst rect are used to calculate the src rect
    // (the intersect is taken to calculate non overlapping areas)
    // dst rect size is modified if there are non overlapping areas
    // and src rect size aswell

    // if user src rect is larger, dst rect size is only modified
    // if sample dst rect is larger, src rect size is oly modified

    RECT user_src_rect, user_dst_rect;
    user_src_rect = user_params.source_rect;
    user_dst_rect = user_params.dest_rect;

    RECT sample_src_rect, sample_dst_rect;
    sample_src_rect = stream_params.source_rect;
    sample_dst_rect = stream_params.dest_rect;

    double sample_src_width, sample_src_height;
    sample_src_width = sample_src_rect.right - sample_src_rect.left;
    sample_src_height = sample_src_rect.bottom - sample_src_rect.top;

    double sample_dst_width, sample_dst_height;
    sample_dst_width = sample_dst_rect.right - sample_dst_rect.left;
    sample_dst_height = sample_dst_rect.bottom - sample_dst_rect.top;

    double user_src_width, user_src_height;
    user_src_width = user_src_rect.right - user_src_rect.left;
    user_src_height = user_src_rect.bottom - user_src_rect.top;

    double user_dst_width, user_dst_height;
    user_dst_width = user_dst_rect.right - user_dst_rect.left;
    user_dst_height = user_dst_rect.bottom - user_dst_rect.top;

    RECT nonoverlapping_rect;
    nonoverlapping_rect.top = user_src_rect.top - sample_dst_rect.top;
    nonoverlapping_rect.left = user_src_rect.left - sample_dst_rect.left;
    // these are negative
    nonoverlapping_rect.bottom = user_src_rect.bottom - sample_dst_rect.bottom;
    nonoverlapping_rect.right = user_src_rect.right - sample_dst_rect.right;

    src_rect = sample_src_rect;
    dst_rect = user_dst_rect;

    if(nonoverlapping_rect.top > 0)
        src_rect.top += (LONG)(sample_src_height * (nonoverlapping_rect.top / sample_dst_height));
    else if(nonoverlapping_rect.top < 0)
        dst_rect.top -= (LONG)(user_dst_height * (nonoverlapping_rect.top / user_src_height));
    if(nonoverlapping_rect.left > 0)
        src_rect.left += (LONG)(sample_src_width * (nonoverlapping_rect.left / sample_dst_width));
    else if(nonoverlapping_rect.left < 0)
        dst_rect.left -= (LONG)(user_dst_width * (nonoverlapping_rect.left / user_src_width));
    if(nonoverlapping_rect.bottom < 0)
        src_rect.bottom += (LONG)(sample_src_height * (nonoverlapping_rect.bottom / sample_dst_height));
    else if(nonoverlapping_rect.bottom > 0)
        dst_rect.bottom -= (LONG)(user_dst_height * (nonoverlapping_rect.bottom / user_src_height));
    if(nonoverlapping_rect.right < 0)
        src_rect.right += (LONG)(sample_src_width * (nonoverlapping_rect.right / sample_dst_width));
    else if(nonoverlapping_rect.right > 0)
        dst_rect.right -= (LONG)(user_dst_width * (nonoverlapping_rect.right / user_src_width));

    // TODO: maybe add checks for valid dst rect ranges

    return (src_rect.top < src_rect.bottom && src_rect.left < src_rect.right) && 
        (src_rect.top >= sample_src_rect.top && src_rect.left >= sample_src_rect.left &&
        src_rect.right <= sample_src_rect.right && src_rect.bottom <= sample_src_rect.bottom);
}

void stream_videoprocessor::processing_cb(void*)
{
    // TODO: any kind of parameter changing should only happen by reinitializing
    // the streams;
    // actually, some parameters can be changed by setting the packet number point
    // where new parameters will apply

    HRESULT hr = S_OK;
    bool blit = false;

    // lock the output sample
    media_sample_texture sample, temp_sample;
    /*media_sample_view_t sample_view, temp_sample_view;*/
    time_unit timestamp = std::numeric_limits<time_unit>::max();

    // rps are equivalent in every input stream
    request_packet rp = this->input_streams[0].first.rp;

    UINT j = 0;
    bool blend_output = false;
    int output_index = 0;
    // construct a list of streams that will be blit onto output surface;
    // this must be used because for some reason atiumdva has a null pointer read violation
    // if setting enabled state of a stream to false
    for(auto it = this->input_streams.begin(); it != this->input_streams.end(); it++)
    {
        bool blit_stream = false;
        CComPtr<ID3D11VideoProcessorInputView> input_view;
        CComPtr<ID3D11Texture2D> texture = it->first.sample_view.buffer->texture;
        const media_sample_videoprocessor::params_t& params = 
            it->first.sample_view.params;
        media_sample_videoprocessor::params_t user_params;
        D3D11_VIDEO_PROCESSOR_STREAM native_params;

        if(it->first.user_params)
            it->first.user_params->get_params(user_params);
        else
            user_params = params;

        // use the earliest timestamp;
        // actually, max must be used so that the timestamp stays incremental
        // (using max only applies to displaycapture where the buffers are shared between streams)
        timestamp = rp.request_time;//std::min(timestamp, it->first.sample_view.timestamp);

        if(blend_output)
        {
            // add the blitted output to input
            bool blit_stream = false;
            media_sample_videoprocessor::params_t params;
            D3D11_VIDEO_PROCESSOR_STREAM native_params;
            params.enable_alpha = false;
            params.source_rect = this->output_target_rect;
            params.dest_rect = this->output_target_rect;
            temp_sample.buffer = this->output_buffer[output_index];
            /*temp_sample_view.attach(this->output_buffer[output_index], view_lock_t::READ_LOCK_BUFFERS);*/
            /*temp_sample_view.reset(new media_sample_view(this->output_buffer[output_index]));*/
            CHECK_HR(hr = this->set_input_stream(
                params, params, this->output_buffer[output_index]->texture,
                native_params, j, blit_stream));

            assert_(blit_stream);
            this->streams.push_back(native_params);
            j++;

            blend_output = false;
            output_index = (output_index + 1) % 2;
        }

        CHECK_HR(hr = 
            this->set_input_stream(params, user_params, texture, native_params, j, blit_stream));
        if(!blit_stream)
            continue;

        this->streams.push_back(native_params);
        j++;

        if(this->streams.size() >= this->transform->max_input_streams()
            && (it + 1) != this->input_streams.end())
        {
            assert_(this->streams.size() == this->transform->max_input_streams());

            blit = true;
            // temp lock for the output buffer
            media_sample_texture sample_view(this->output_buffer[output_index]);
            /*sample_view.attach(this->output_buffer[output_index], view_lock_t::READ_LOCK_BUFFERS);*/
            /*media_sample_view_t sample_view(new media_sample_view(this->output_buffer[output_index]));*/
            CHECK_HR(hr = this->blit(this->streams, this->output_view[output_index]));
            /*temp_sample_view.detach();*/
            /*temp_sample_view.reset();*/

            blend_output = true;
            this->release_input_streams(this->streams);
            j = 0;
        }
    }

    CHECK_HR(hr = this->blit(this->streams, this->output_view[output_index]));

    // on failure the videoprocessor sends the sample and requests a reinitialization
done:
    if(FAILED(hr))
    {
        PRINT_ERROR(hr);
        this->transform->request_reinitialization(this->transform->ctrl_pipeline);
    }

    sample.buffer = this->output_buffer[output_index];
    sample.timestamp = timestamp;

    this->release_input_streams(this->streams);

    // reset the sample view from the input stream packets so it is unlocked;
    // reset the rps so that there aren't any circular dependencies at shutdown
    for(auto it = this->input_streams.begin(); it != this->input_streams.end(); it++)
    {
        it->first.sample_view.buffer = NULL;
        /*it->first.sample_view.detach();*/
        it->first.sample_view.timestamp = -1;
        it->first.rp = request_packet();
    }

    this->transform->session->give_sample(this, sample, rp, false);
    /*return;*/

//done:
//    this->release_input_streams(this->streams);
//    if(FAILED(hr))
//        throw HR_EXCEPTION(hr);
}

void stream_videoprocessor::add_input_stream(
    const media_stream* stream,
    const stream_videoprocessor_controller_t& user_params)
{
    this->input_streams.push_back(std::make_pair(packet(), stream));
    this->input_streams.back().first.user_params = user_params;
}

media_stream::result_t stream_videoprocessor::request_sample(
    request_packet& rp, const media_stream*)
{
    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_videoprocessor::process_sample(
    const media_sample& sample_view_, request_packet& rp, const media_stream* prev_stream)
{
    const media_sample_videoprocessor& sample_view = 
        reinterpret_cast<const media_sample_videoprocessor&>(sample_view_);

    CComPtr<ID3D11Texture2D> texture = sample_view.buffer->texture;

    /*media_sample_view_videoprocessor_t sample_view = cast<media_sample_view_videoprocessor>(sample_view_);
    assert_(sample_view);
    CComPtr<ID3D11Texture2D> texture = sample_view->texture_buffer->texture;*/

    // this function needs to be locked because multiple inputs
    // may be running simultaneously
    std::unique_lock<std::recursive_mutex> lock(this->mutex);

    {
        // TODO: sample view could include an index so that the input stream
        // doesn't need to be searched for
        auto it = std::find_if(this->input_streams.begin(), this->input_streams.end(), 
            [prev_stream](const input_streams_t& e) {
            return e.second == prev_stream && e.first.sample_view.timestamp == -1;});
        assert_(it != this->input_streams.end());

        it->first.rp = rp;
        it->first.sample_view = sample_view;
        this->samples_received++;

        assert_(this->samples_received <= (int)this->input_streams.size());
        if(this->samples_received == (int)this->input_streams.size())
        {
            this->samples_received = 0;
            this->processing_cb(NULL);
        }
    }

    return OK;
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


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


media_sample_videoprocessor::media_sample_videoprocessor(const media_buffer_texture_t& texture_buffer) :
    media_sample_texture(texture_buffer)
{
}

media_sample_videoprocessor::media_sample_videoprocessor(
    const params_t& params, const media_buffer_texture_t& texture_buffer) :
    media_sample_texture(texture_buffer),
    params(params)
{
}