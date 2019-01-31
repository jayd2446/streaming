#include "source_vidcap.h"
#include "transform_h264_encoder.h"
#include "transform_videomixer.h"
#include "assert.h"

#undef max
#undef min

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

source_vidcap::source_vidcap(const media_session_t& session) :
    source_base(session),
    buffer_pool_texture(new buffer_pool_texture_t),
    buffer_pool_video_frames(new buffer_pool_video_frames_t),
    captured_video(new media_sample_video_mixer_frames)
{
    this->capture_callback.Attach(new async_callback_t(&source_vidcap::capture_cb));
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
}

source_vidcap::stream_source_base_t source_vidcap::create_derived_stream()
{
    return stream_vidcap_t(new stream_vidcap(this->shared_from_this<source_vidcap>()));
}

bool source_vidcap::get_samples_end(const request_t& request, frame_unit& end)
{
    scoped_lock lock(this->captured_video_mutex);
    if(this->captured_video->frames.empty())
        return false;

    end = this->captured_video->end;
    return true;
}

void source_vidcap::make_request(request_t& request, frame_unit frame_end)
{
    media_sample_video_mixer_frames_t captured_video;
    {
        buffer_pool_video_frames_t::scoped_lock lock(this->buffer_pool_video_frames->mutex);
        captured_video = this->buffer_pool_video_frames->acquire_buffer();
        captured_video->initialize();
    }

    // null args are passed if the source has too new samples for the request;
    // if this happens on drain, transform_mixer will emit a warning, which can be ignored

    // TODO: decide what to do if the source has samples with greater sample times than the
    // frame_end;
    // probably should just set the frame end without a sample: skipping frames
    // (source base could do that)
    // (or serve null samples);
    // this can be easily reproduced by switching to an another scene before vidcap
    // has fetched the first frame

    scoped_lock lock(this->captured_video_mutex);
    const bool moved = this->captured_video->move_frames_to(captured_video.get(), frame_end);

    media_component_videomixer_args_t& args = request.sample;
    args = std::make_optional<media_component_videomixer_args>();

    args->frame_end = frame_end;
    // frames are simply skipped if there is no sample for the args
    if(moved)
    {
        args->sample = std::move(captured_video);
        // the sample must not be empty
        assert_(!args->sample->frames.empty());
    }

    //if(this->captured_video->move_frames_to(captured_video.get(), frame_end))
    //{
    //    media_component_videomixer_args_t& args = request.sample;
    //    args = std::make_optional<media_component_videomixer_args>();

    //    args->frame_end = frame_end;
    //    args->sample = std::move(captured_video);
    //    // the sample must not be empty
    //    assert_(!args->sample->frames.empty());
    //}
}

void source_vidcap::dispatch(request_t& request)
{
    this->session->give_sample(request.stream, request.sample.has_value() ?
        &(*request.sample) : NULL, request.rp);
}

void source_vidcap::capture_cb(void*)
{
    HRESULT hr = S_OK;

    //  this is assumed to be singlethreaded

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
        media_sample_video_mixer_frame frame;

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
        frame.dur = 1;
        {
            buffer_pool_texture_t::scoped_lock lock(this->buffer_pool_texture->mutex);
            frame.buffer = this->buffer_pool_texture->acquire_buffer();
            frame.buffer->initialize(texture);
        }

        if(frame.buffer)
        {
            frame.params.source_rect.top = frame.params.source_rect.left = 0.f;
            frame.params.source_rect.right = (FLOAT)this->frame_width;
            frame.params.source_rect.bottom = (FLOAT)this->frame_height;
            frame.params.dest_rect = frame.params.source_rect;
            frame.params.source_m = frame.params.dest_m = D2D1::Matrix3x2F::Identity();
        }

        // add the frame to captured frames
        {
            scoped_lock lock(this->captured_video_mutex);

            // fill possible skipped frames with the last frame buffer
            const frame_unit skipped_frames_dur = (frame.pos - this->last_captured_frame_end);
            if(skipped_frames_dur > 0)
            {
                media_sample_video_mixer_frame duplicate_frame;
                duplicate_frame.pos = this->last_captured_frame_end;
                duplicate_frame.dur = skipped_frames_dur;
                duplicate_frame.buffer = this->last_captured_buffer;

                if(duplicate_frame.buffer)
                {
                    duplicate_frame.params.source_rect.top = 
                        duplicate_frame.params.source_rect.left = 0.f;
                    duplicate_frame.params.source_rect.right = (FLOAT)this->frame_width;
                    duplicate_frame.params.source_rect.bottom = (FLOAT)this->frame_height;
                    duplicate_frame.params.dest_rect = duplicate_frame.params.source_rect;
                    duplicate_frame.params.source_m = duplicate_frame.params.dest_m = 
                        D2D1::Matrix3x2F::Identity();
                }

                this->captured_video->end = std::max(
                    this->captured_video->end, duplicate_frame.pos + duplicate_frame.dur);
                this->captured_video->frames.push_back(std::move(duplicate_frame));
            }

            this->last_captured_buffer = frame.buffer;
            this->last_captured_frame_end = frame.pos + frame.dur;

            // add the new frame
            this->captured_video->end = std::max(
                this->captured_video->end, frame.pos + frame.dur);
            this->captured_video->frames.push_back(std::move(frame));

            // TODO: source vidcap must serve silent samples before the first sample,
            // otherwise the other sources buffer too much

            // because the end is accessed here, the captured video must not be empty
            assert_(!this->captured_video->frames.empty());
            // keep the frames buffer within the limits
            if(this->captured_video->move_frames_to(NULL,
                this->captured_video->end - maximum_buffer_size))
            {
                std::cout << "source_vidcap buffer limit reached, excess frames discarded" << std::endl;
            }
        }
    }

    CHECK_HR(hr = this->queue_new_capture());

done:
    // TODO: source_vidcap should handle a case where the device is being used by another app

    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
}

HRESULT source_vidcap::queue_new_capture()
{
    HRESULT hr = S_OK;
    CHECK_HR(hr = this->capture_callback->mf_put_work_item(
        this->shared_from_this<source_vidcap>()));
done:
    return hr;
}

void source_vidcap::initialize(const control_class_t& ctrl_pipeline,
    const CComPtr<ID3D11Device>& d3d11dev,
    const std::wstring& symbolic_link)
{
    HRESULT hr = S_OK;

    assert_(!this->device);

    this->source_base::initialize(
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);

    this->d3d11dev = d3d11dev;
    this->symbolic_link = symbolic_link;
    this->ctrl_pipeline = ctrl_pipeline;

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

    // new capture is queued on component start, so that
    // the last_captured_frame_end variable isn't accessed before assigned

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_vidcap::stream_vidcap(const source_vidcap_t& source) :
    stream_source_base(source),
    source(source)
{
}

void stream_vidcap::on_component_start(time_unit t)
{
    HRESULT hr = S_OK;

    this->source->last_captured_frame_end = convert_to_frame_unit(t,
        transform_h264_encoder::frame_rate_num,
        transform_h264_encoder::frame_rate_den);

    CHECK_HR(hr = this->source->queue_new_capture());

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
}