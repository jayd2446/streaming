#include "source_vidcap.h"
#include "transform_h264_encoder.h"
#include "transform_videomixer.h"
#include "assert.h"

#undef max
#undef min

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

source_vidcap::source_vidcap(const media_session_t& session) :
    media_source(session),
    started(false),
    buffer_pool_texture(new buffer_pool_texture_t),
    buffer_pool_video_frames(new buffer_pool_video_frames_t),
    captured_video(new media_sample_video_frames),
    serve_callback_event(CreateEvent(NULL, TRUE, FALSE, NULL))
{
    HRESULT hr = S_OK;

    if(!this->serve_callback_event)
        CHECK_HR(hr = E_UNEXPECTED);

    this->capture_callback.Attach(new async_callback_t(&source_vidcap::capture_cb));
    this->serve_callback.Attach(new async_callback_t(&source_vidcap::serve_cb));

    CHECK_HR(hr = MFCreateAsyncResult(NULL, &this->serve_callback->native,
        NULL, &this->serve_callback_result));

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

source_vidcap::~source_vidcap()
{
    {
        buffer_pool_video_frames_t::scoped_lock lock(this->buffer_pool_video_frames->mutex);
        this->buffer_pool_video_frames->dispose();
    }
    {
        buffer_pool_texture_t::scoped_lock lock(this->buffer_pool_texture->mutex);
        this->buffer_pool_texture->dispose();
    }

    std::cout << "stopping vidcap..." << std::endl;

    HRESULT hr = S_OK;

    // canceling the work item is important so that the data associated with the callback
    // is released
    if(this->serve_in_wait_queue)
        hr = MFCancelWorkItem(this->serve_callback_key);
}

HRESULT source_vidcap::queue_new_capture()
{
    HRESULT hr = S_OK;
    CHECK_HR(hr = this->capture_callback->mf_put_work_item(
        this->shared_from_this<source_vidcap>()));

done:
    return hr;
}

void source_vidcap::serve_cb(void*)
{
    std::unique_lock<std::mutex> lock(this->requests_mutex);
    HRESULT hr = S_OK;

    while(!this->requests.empty())
    {
        media_sample_video_frames_t captured_video;
        media_component_videomixer_args_t args;
        {
            scoped_lock lock(this->captured_video_mutex);
            const frame_unit frame_end = convert_to_frame_unit(
                this->requests.front().rp.request_time,
                transform_h264_encoder::frame_rate_num,
                transform_h264_encoder::frame_rate_den);

            const bool drain = this->requests.front().sample.drain;

            // do not serve the request if there's not enough data
            if(drain && this->captured_video->end < frame_end)
                break;

            if(drain)
                std::cout << "drain on source_vidcap" << std::endl;

            {
                buffer_pool_video_frames_t::scoped_lock lock(this->buffer_pool_video_frames->mutex);
                captured_video = this->buffer_pool_video_frames->acquire_buffer();
                captured_video->initialize();
            }

            if(this->captured_video->move_frames_to(captured_video.get(), frame_end) || drain)
            {
                // TODO: decide if frame info should be retrieved from texture desc
                stream_videomixer_controller::params_t params;
                params.source_rect.top = params.source_rect.left = 0.f;
                params.source_rect.right = (FLOAT)this->frame_width;
                params.source_rect.bottom = (FLOAT)this->frame_height;
                params.dest_rect = params.source_rect;
                params.source_m = params.dest_m = D2D1::Matrix3x2F::Identity();

                args = std::make_optional<media_component_videomixer_args>(params);
                args->frame_end = drain ? frame_end : captured_video->end;
                args->sample = std::move(captured_video);
            }
        }

        request_queue::request_t request = std::move(this->requests.front());
        this->requests.pop();

        lock.unlock();
        this->session->give_sample(request.stream, args.has_value() ? &(*args) : NULL, request.rp);
        lock.lock();
    }

    ResetEvent(this->serve_callback_event);

    CHECK_HR(hr = this->serve_callback->mf_put_waiting_work_item(
        this->shared_from_this<source_vidcap>(), this->serve_callback_event, 0,
        this->serve_callback_result, &this->serve_callback_key));

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
}

void source_vidcap::capture_cb(void*)
{
    HRESULT hr = S_OK;

    DWORD stream_index, flags;
    LONGLONG timestamp;
    CComPtr<IMFSample> sample;

    // TODO: drain here
    CHECK_HR(hr = this->source_reader->ReadSample(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0, &stream_index, &flags, &timestamp, &sample));

    if(flags & MF_SOURCE_READERF_ENDOFSTREAM)
    {
        std::cout << "end of stream" << std::endl;
    }
    if(flags & MF_SOURCE_READERF_NEWSTREAM)
    {
        std::cout << "new stream" << std::endl;
    }
    if(flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)
    {
        std::cout << "native mediatype changed" << std::endl;
    }
    if(flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
    {
        std::cout << "current mediatype changed" << std::endl;
    }
    if(flags & MF_SOURCE_READERF_STREAMTICK)
    {
        std::cout << "stream tick" << std::endl;
    }

    if(sample)
    {
        CComPtr<IMFMediaBuffer> buffer;
        CComPtr<IMFDXGIBuffer> dxgi_buffer;
        CComPtr<ID3D11Texture2D> texture;
        presentation_time_source_t time_source = this->session->get_time_source();
        media_sample_video_frame frame;

        if(!time_source)
        {
            std::cout << "time source was not initialized" << std::endl;
            goto done;
        }

        CHECK_HR(hr = sample->GetBufferByIndex(0, &buffer));
        CHECK_HR(hr = buffer->QueryInterface(&dxgi_buffer));
        CHECK_HR(hr = dxgi_buffer->GetResource(__uuidof(ID3D11Texture2D), (LPVOID*)&texture));
        
        CHECK_HR(hr = sample->GetSampleTime(&timestamp));

        // make frame
        frame.pos = convert_to_frame_unit(
            time_source->system_time_to_time_source((time_unit)timestamp),
            transform_h264_encoder::frame_rate_num,
            transform_h264_encoder::frame_rate_den);
        {
            buffer_pool_texture_t::scoped_lock lock(this->buffer_pool_texture->mutex);
            frame.buffer = this->buffer_pool_texture->acquire_buffer();
            frame.buffer->texture = texture;
        }

        // add the frame to frames
        {
            scoped_lock lock(this->captured_video_mutex);
            this->captured_video->end = std::max(
                this->captured_video->end, frame.pos + frame.dur);
            this->captured_video->frames.push_back(std::move(frame));

            // keep the buffer within the limits
            if(this->captured_video->move_frames_to(NULL,
                this->captured_video->end - maximum_buffer_size))
            {
                std::cout << "source_vidcap buffer limit reached, excess frames discarded" << std::endl;
            }
        }
    }

done:
    if(FAILED(hr = this->queue_new_capture()) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
}

void source_vidcap::initialize(const control_class_t& ctrl_pipeline, 
    const CComPtr<ID3D11Device>& d3d11dev,
    const std::wstring& symbolic_link)
{
    assert_(!this->device);

    this->d3d11dev = d3d11dev;
    this->symbolic_link = symbolic_link;
    this->ctrl_pipeline = ctrl_pipeline;

    HRESULT hr = S_OK;
    CComPtr<IMFAttributes> attributes;
    CComPtr<IMFMediaType> output_type;

    CHECK_HR(hr = MFCreateDXGIDeviceManager(&this->reset_token, &this->devmngr));
    CHECK_HR(hr = this->devmngr->ResetDevice(this->d3d11dev, this->reset_token));

    // configure device source
    CHECK_HR(hr = MFCreateAttributes(&attributes, 2));
    CHECK_HR(hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
    CHECK_HR(hr = attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
        this->symbolic_link.c_str()));

    // configure source reader
    CHECK_HR(hr = MFCreateAttributes(&this->source_reader_attributes, 1));
    CHECK_HR(hr = this->source_reader_attributes->SetUINT32(
        MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    CHECK_HR(hr = this->source_reader_attributes->SetUnknown(
        MF_SOURCE_READER_D3D_MANAGER, this->devmngr));
    CHECK_HR(hr = this->source_reader_attributes->SetUINT32(
        MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
    CHECK_HR(hr = this->source_reader_attributes->SetUINT32(
        MF_READWRITE_DISABLE_CONVERTERS, FALSE));

    // create device source and source reader
    CHECK_HR(hr = MFCreateDeviceSource(attributes, &this->device));
    CHECK_HR(hr = MFCreateSourceReaderFromMediaSource(this->device,
        this->source_reader_attributes, &this->source_reader));

    // set the output format for source reader
    CHECK_HR(hr = MFCreateMediaType(&output_type));
    CHECK_HR(hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32));
    CHECK_HR(hr = MFSetAttributeRatio(output_type, MF_MT_FRAME_RATE,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den));
    // TODO: decide if should set frame size
    CHECK_HR(hr = output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK_HR(hr = MFSetAttributeRatio(output_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

    CHECK_HR(hr = this->source_reader->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        NULL, output_type));
    CHECK_HR(hr = this->source_reader->GetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        &this->output_type));
    CHECK_HR(hr = MFGetAttributeSize(this->output_type, MF_MT_FRAME_SIZE,
        &this->frame_width, &this->frame_height));

    this->started = true;
    CHECK_HR(hr = this->queue_new_capture());

    CHECK_HR(hr = this->serve_callback->mf_put_waiting_work_item(
        this->shared_from_this<source_vidcap>(), this->serve_callback_event, 0,
        this->serve_callback_result, &this->serve_callback_key));
    this->serve_in_wait_queue = true;

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

media_stream_t source_vidcap::create_stream(presentation_clock_t&& clock)
{
    stream_vidcap_t stream(new stream_vidcap(this->shared_from_this<source_vidcap>()));
    stream->register_sink(clock);

    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_vidcap::stream_vidcap(const source_vidcap_t& source) :
    media_stream_clock_sink(source.get()),
    source(source),
    drain_point(std::numeric_limits<time_unit>::min())
{
}

void stream_vidcap::on_stream_stop(time_unit t)
{
    this->drain_point = t;
}

media_stream::result_t stream_vidcap::request_sample(const request_packet& rp, const media_stream*)
{
    source_vidcap::scoped_lock lock(this->source->requests_mutex);
    source_vidcap::request_queue::request_t request;
    request.rp = rp;
    request.stream = this;
    request.sample.drain = (this->drain_point == rp.request_time);
    this->source->requests.push(request);

    return OK;
}

media_stream::result_t stream_vidcap::process_sample(
    const media_component_args*, const request_packet&, const media_stream*)
{
    source_vidcap::scoped_lock lock(this->source->requests_mutex);
    SetEvent(this->source->serve_callback_event);

    return OK;
}