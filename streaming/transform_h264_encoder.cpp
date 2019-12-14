#include "transform_h264_encoder.h"
#include <Mferror.h>
#include <initguid.h>
#include <evr.h>
#include <iostream>
#include "assert.h"
#include "IUnknownImpl.h"

#pragma comment(lib, "dxguid.lib")

//void CHECK_HR(HRESULT hr)
//{
//    if(FAILED(hr))
//        throw HR_EXCEPTION(hr);
//}
#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
#undef max
#undef min

DEFINE_GUID(media_sample_tracker_guid,
    0xd84fe03a, 0xcb44, 0x43ec, 0x10, 0xac, 0x94, 0x00, 0xb, 0xcc, 0xef, 0x38);

// wraps a texture sample
class media_buffer_wrapper : public IMFMediaBuffer, IUnknownImpl
{
private:
    const bool use_system_memory;
    // memory corruption occurs if media_buffer_wrapper outlives media_sample_tracker
    media_buffer_texture_t buffer;
    CComPtr<IMFMediaBuffer> media_buffer;
public:
    explicit media_buffer_wrapper(const context_mutex_t& /*context_mutex*/,
        const media_buffer_texture_t& buffer,
        const CComPtr<IMFMediaBuffer>& media_buffer,
        bool use_system_memory = false) :
        buffer(buffer), media_buffer(media_buffer), use_system_memory(use_system_memory)
    {
    }

    ULONG STDMETHODCALLTYPE AddRef() {return IUnknownImpl::AddRef();}
    ULONG STDMETHODCALLTYPE Release() {return IUnknownImpl::Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv)
    {
        return this->media_buffer->QueryInterface(riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE Lock(BYTE **ppbBuffer, DWORD *pcbMaxLength, DWORD *pcbCurrentLength)
    {
        return this->media_buffer->Lock(ppbBuffer, pcbMaxLength, pcbCurrentLength);
    }
    HRESULT STDMETHODCALLTYPE Unlock()
    {
        return this->media_buffer->Unlock();
    }
    HRESULT STDMETHODCALLTYPE GetCurrentLength(DWORD *pcbCurrentLength)
    {
        return this->media_buffer->GetCurrentLength(pcbCurrentLength);
    }
    HRESULT STDMETHODCALLTYPE SetCurrentLength(DWORD cbCurrentLength)
    {
        return this->media_buffer->SetCurrentLength(cbCurrentLength);
    }
    HRESULT STDMETHODCALLTYPE GetMaxLength(DWORD *pcbMaxLength)
    {
        return this->media_buffer->GetMaxLength(pcbMaxLength);
    }
};

// ensures the lifetime of texture sample so that it won't be deleted while being used
// by the encoder
class media_sample_tracker : public IUnknown, IUnknownImpl
{
public:
    CComPtr<media_buffer_wrapper> buffer_wrapper;

    explicit media_sample_tracker(const CComPtr<media_buffer_wrapper>& buffer_wrapper) :
        buffer_wrapper(buffer_wrapper) {}
    ULONG STDMETHODCALLTYPE AddRef() { return IUnknownImpl::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() { return IUnknownImpl::Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv)
    {
        if(!ppv)
            return E_POINTER;
        if(riid == __uuidof(IUnknown))
            *ppv = static_cast<IUnknown*>(this);
        else
        {
            *ppv = NULL;
            return E_NOINTERFACE;
        }

        this->AddRef();
        return S_OK;
    }
};

transform_h264_encoder::transform_h264_encoder(const media_session_t& session, 
    context_mutex_t context_mutex) :
    media_component(session),
    encoder_requests(0),
    last_time_stamp(std::numeric_limits<time_unit>::min()),
    last_time_stamp2(std::numeric_limits<time_unit>::min()),
    last_packet(std::numeric_limits<int>::min()),
    context_mutex(context_mutex),
    use_system_memory(false),
    software(false),
    draining(false),
    first_sample(true),
    time_shift(-1),
    buffer_pool_h264_frames(new buffer_pool_h264_frames_t),
    buffer_pool_memory(new buffer_pool_memory_t),
    dispatcher(new request_dispatcher)
{
    this->events_callback.Attach(new async_callback_t(&transform_h264_encoder::events_cb));
}

transform_h264_encoder::~transform_h264_encoder()
{
    HRESULT hr = S_OK;
    CComPtr<IMFShutdown> shutdown;
    if(this->encoder && SUCCEEDED(hr = this->encoder->QueryInterface(&shutdown)))
        hr = shutdown->Shutdown();

    {
        buffer_pool_memory_t::scoped_lock lock(this->buffer_pool_memory->mutex);
        this->buffer_pool_memory->dispose();
    }
    {
        buffer_pool_h264_frames_t::scoped_lock lock(this->buffer_pool_h264_frames->mutex);
        this->buffer_pool_h264_frames->dispose();
    }
}

HRESULT transform_h264_encoder::set_input_stream_type()
{
    HRESULT hr = S_OK;

    CComPtr<IMFMediaType> input_type;
    CHECK_HR(hr = MFCreateMediaType(&input_type));
    CHECK_HR(hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));

    CHECK_HR(hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    /*CHECK_HR(hr = input_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    CHECK_HR(hr = input_type->SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_Cosited));
    CHECK_HR(hr = input_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));*/
    /*CHECK_HR(hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32));*/

    CHECK_HR(hr = MFSetAttributeRatio(input_type, MF_MT_FRAME_RATE, 
        (UINT32)this->session->frame_rate_num, (UINT32)this->session->frame_rate_den));
    CHECK_HR(hr = MFSetAttributeSize(input_type, MF_MT_FRAME_SIZE, 
        this->frame_width, this->frame_height));
    CHECK_HR(hr = input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK_HR(hr = input_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    CHECK_HR(hr = MFSetAttributeRatio(input_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

    CHECK_HR(hr = this->encoder->SetInputType(this->input_id, input_type, 0));
done:
    return hr;
}

HRESULT transform_h264_encoder::set_output_stream_type()
{
    HRESULT hr = S_OK;
    this->output_type = NULL;
    CHECK_HR(hr = MFCreateMediaType(&this->output_type));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AVG_BITRATE, this->avg_bitrate));
    CHECK_HR(hr = MFSetAttributeRatio(this->output_type, MF_MT_FRAME_RATE, 
        this->frame_rate_num, this->frame_rate_den));
    CHECK_HR(hr = MFSetAttributeSize(this->output_type, MF_MT_FRAME_SIZE, 
        this->frame_width, this->frame_height));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    // intel mft only supports main profile
    // (There is no support for Baseline, Extended, or High-10 Profiles.)
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main));
    /*CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));*/
    CHECK_HR(hr = MFSetAttributeRatio(this->output_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

    CHECK_HR(hr = this->encoder->SetOutputType(this->output_id, this->output_type, 0));
done:
    return hr;
}

HRESULT transform_h264_encoder::set_encoder_parameters()
{
    HRESULT hr = S_OK;
    CComPtr<ICodecAPI> codec;
    VARIANT v;

    CHECK_HR(hr = this->encoder->QueryInterface(&codec));

    v.vt = VT_UI4;
    v.ulVal = eAVEncCommonRateControlMode_CBR;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v));
    v.vt = VT_UI4;
    v.ullVal = this->quality_vs_speed;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v));
    v.vt = VT_UI4;
    v.ullVal = this->avg_bitrate;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v));
    /*v.vt = VT_UI4;
    v.ullVal = 1;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v));*/
    if(codec->IsSupported(&CODECAPI_AVLowLatencyMode) == S_OK)
    {
        v.vt = VT_BOOL;
        v.ulVal = VARIANT_FALSE;
        CHECK_HR(hr = codec->SetValue(&CODECAPI_AVLowLatencyMode, &v));
    }
    /*v.vt = VT_UI4;
    v.ulVal = 0;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVEncNumWorkerThreads, &v));*/

done:
    return hr;
}

HRESULT transform_h264_encoder::feed_encoder(const media_sample_video_frame& frame)
{
    HRESULT hr = S_OK;

    CComPtr<IMFMediaBuffer> buffer;
    CComPtr<media_buffer_wrapper> buffer_wrapper;
    CComPtr<IMFSample> sample;
    CComPtr<IUnknown> sample_tracker;

#ifdef _DEBUG
    {
        D3D11_TEXTURE2D_DESC desc;
        frame.buffer->texture->GetDesc(&desc);
        assert_(desc.Width == this->frame_width && desc.Height == this->frame_height);
    }
#endif

    // sample tracker should be used for each texture individually

    // create the input sample buffer
    CHECK_HR(hr = MFCreateDXGISurfaceBuffer(IID_ID3D11Texture2D,
        frame.buffer->texture, 0, FALSE, &buffer));
    buffer_wrapper.Attach(new media_buffer_wrapper(this->context_mutex,
        frame.buffer, buffer, this->use_system_memory));

    assert_(frame.dur == 1);

    time_unit sample_time = convert_to_time_unit(frame.pos,
        this->session->frame_rate_num, this->session->frame_rate_den);
    const time_unit sample_duration = convert_to_time_unit(1,
        this->session->frame_rate_num, this->session->frame_rate_den);

    sample_time -= this->time_shift;
    if(sample_time < 0)
    {
        std::cout << "h264 encoder time shift was off by " << sample_time << std::endl;
        sample_time = 0;
    }

    // create sample
    CHECK_HR(hr = MFCreateSample(&sample));
    CHECK_HR(hr = sample->AddBuffer(buffer_wrapper));
    CHECK_HR(hr = sample->SetSampleTime(sample_time));
    CHECK_HR(hr = sample->SetSampleDuration(sample_duration));
    // the amd encoder probably copies the discontinuity flag to output sample,
    // which might cause problems when the sample is passed to sinkwriter
    //if((request.rp.flags & FLAG_DISCONTINUITY) || this->first_sample)
    //{
    //    /*CHECK_HR(hr = sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE));*/
    //    this->first_sample = false;
    //}
    /*else
        CHECK_HR(hr = sample->SetUINT32(MFSampleExtension_Discontinuity, FALSE));*/
    // add the tracker to the sample
    sample_tracker.Attach(new media_sample_tracker(buffer_wrapper));
    CHECK_HR(hr = sample->SetUnknown(media_sample_tracker_guid, sample_tracker));

    // feed the encoder
    {
        /*std::unique_lock<std::recursive_mutex> lock(*this->context_mutex, std::defer_lock);
        if(!this->use_system_memory)
            lock.lock();*/
        CHECK_HR(hr = this->encoder->ProcessInput(this->input_id, sample, 0));
    }

done:
    return hr;
}

void transform_h264_encoder::events_cb(void* unk)
{
    try
    {
        streaming::check_for_errors();

        IMFAsyncResult* result = (IMFAsyncResult*)unk;
        HRESULT hr = S_OK;
        CComPtr<IMFMediaEvent> media_event;

        // get the event from the event queue
        CHECK_HR(hr = this->event_generator->EndGetEvent(result, &media_event));

        // process the event
        MediaEventType type = MEUnknown;
        HRESULT status = S_OK;
        CHECK_HR(hr = media_event->GetType(&type));
        CHECK_HR(hr = media_event->GetStatus(&status));

        if(type == METransformNeedInput)
        {
            this->encoder_requests++;
            this->serve();
        }
        else if(type == METransformHaveOutput)
        {
            this->process_output_cb(NULL);
        }
        else if(type == METransformDrainComplete)
        {
            media_sample_h264_frames_t out_sample;
            request_t request = this->last_request;
            this->last_request = request_t();
            {
                scoped_lock lock(this->process_output_mutex);
                out_sample = std::move(this->out_sample);
            }

            this->process_request(out_sample, request);
        }
        else if(type == MEError)
        {
            // status has the error code
            throw HR_EXCEPTION(status);
        }
        else
            assert_(false);

        // set callback for the next event
        CHECK_HR(hr = this->event_generator->BeginGetEvent(&this->events_callback->native, NULL));

done:
        if(FAILED(hr))
            throw HR_EXCEPTION(hr);
    }
    catch(streaming::exception e)
    {
        streaming::print_error_and_abort(e.what());
    }
}

bool transform_h264_encoder::extract_frame(media_sample_video_frame& frame, const request_t& request)
{
    assert_(!frame.buffer);

    if(!request.sample.args)
        return true;

    while(!request.sample.args->sample->get_frames().empty() && !frame.buffer)
    {
        // TODO: h264 encoder should copy the frames container and modify that
        // TODO: vector should be used for media_sample_video_frames_template
        frame = const_cast<media_sample_video_frame&>(
            request.sample.args->sample->get_frames().front());

        const_cast<media_sample_video_frames::samples_t&>(
            request.sample.args->sample->get_frames()).pop_front();
    }

    return request.sample.args->sample->get_frames().empty();
}

bool transform_h264_encoder::on_serve(request_queue::request_t& request)
{
    // TODO: software encoder drain

    // output is only attached to requests that contain valid input data;
    // this really doesn't pose a problem, because the encoder itself outputs frames
    // only when it has been given enough input frames

    HRESULT hr = S_OK;

    const bool not_served_request = !request.sample.already_served;
    media_sample_video_frame video_frame;
    const bool pop_request = this->extract_frame(video_frame, request);

    // there must be a valid texture if the buffer is present
    assert_(!video_frame.buffer || video_frame.buffer->texture);

    // feed the encoder
    if(video_frame.buffer)
    {
        const time_unit timestamp = convert_to_time_unit(video_frame.pos,
            this->session->frame_rate_num, this->session->frame_rate_den);
        if(timestamp <= this->last_time_stamp && timestamp >= 0)
        {
            std::cout << "timestamp error in transform_h264_encoder::processing_cb" << std::endl;
            assert_(false);
        }

    back:
        hr = this->feed_encoder(video_frame);

        if(timestamp >= 0)
            this->last_time_stamp = timestamp;

        if(!this->software)
            this->encoder_requests--;
        else if(hr == MF_E_NOTACCEPTING)
        {
            this->process_output_cb(NULL);
            goto back;
        }
        else if(SUCCEEDED(hr))
        {
            DWORD status;
            CHECK_HR(hr = this->encoder->GetOutputStatus(&status));
            if(status & MFT_OUTPUT_STATUS_SAMPLE_READY)
                this->process_output_cb(NULL);
        }
        CHECK_HR(hr);
        assert_(this->encoder_requests >= 0);
    }

    if(pop_request && not_served_request)
    {
        // event callback will dispatch the last request
        if(!request.sample.drain || this->software)
        {
            media_sample_h264_frames_t out_sample;
            {
                scoped_lock lock(this->process_output_mutex);
                out_sample = std::move(this->out_sample);
            }

            this->process_request(out_sample, request);
        }
        else
        {
            std::cout << "drain on h264 encoder" << std::endl;
            this->last_request = request;
            this->draining = true;
            CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
        }
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);

    return pop_request;
}

transform_h264_encoder::request_queue::request_t* transform_h264_encoder::next_request()
{
    request_queue::request_t* request = this->requests.get();
    if(request && (this->encoder_requests || this->software))
        return request;
    else
        return NULL;
}

void transform_h264_encoder::process_request(
    const media_sample_h264_frames_t& sample, request_t& request)
{
    media_component_h264_video_args_t args;

    assert_(!sample || !sample->frames.empty());

    if(sample)
    {
        args = std::make_optional<media_component_h264_video_args>();
        args->sample = sample;
        args->software = this->software;
    }

    this->last_packet = request.rp.packet_number;

    // reset the sample so that the contained buffer can be reused
    // (optional)
    request.sample.args.reset();

    request_dispatcher::request_t dispatcher_request;
    dispatcher_request.stream = request.stream;
    dispatcher_request.rp = request.rp;
    dispatcher_request.sample = args;

    this->dispatcher->dispatch_request(std::move(dispatcher_request),
        [this_ = this->shared_from_this<transform_h264_encoder>()](
            request_dispatcher::request_t& request)
    {
        this_->session->give_sample(request.stream,
            request.sample.has_value() ? &(*request.sample) : NULL, request.rp);
    });
}

bool transform_h264_encoder::process_output(CComPtr<IMFSample>& sample)
{
    HRESULT hr = S_OK;

    const DWORD mft_provides_samples =
        this->output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;

    MFT_OUTPUT_DATA_BUFFER output = {0};
    DWORD status = 0;
    CComPtr<IMFMediaBuffer> buffer;

    if(mft_provides_samples)
        output.pSample = NULL;
    else
    {
        media_buffer_memory_t buffer;

        CHECK_HR(hr = MFCreateSample(&sample));
        {
            // TODO: the sink writer should call a place marker method for knowing
            // when the output buffer is safe to reuse;
            // currently, a new buffer must be allocated each time
            buffer_pool_memory_t::scoped_lock lock(this->buffer_pool_memory->mutex);
            buffer.reset(new media_buffer_memory);
            /*buffer = this->buffer_pool_memory->acquire_buffer();*/
            buffer->initialize(this->output_stream_info.cbSize);
        }
        
        CHECK_HR(hr = sample->AddBuffer(buffer->buffer));
        output.pSample = sample;
    }

    output.dwStreamID = this->output_id;
    output.dwStatus = 0;
    output.pEvents = NULL;

    {
        // TODO: decide if context mutex is really needed here
        /*std::unique_lock<std::recursive_mutex> lock(*this->context_mutex, std::defer_lock);
        if(!this->use_system_memory)
            lock.lock();*/
        hr = this->encoder->ProcessOutput(0, 1, &output, &status);
    }

    // the output stream type will be the exact same one, but for some reason
    // intel mft requires resetting the output type
    if(FAILED(hr) && output.dwStatus == MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE)
        CHECK_HR(hr = this->set_output_stream_type())
    else
        CHECK_HR(hr)

done:
    if(!sample)
        sample.Attach(output.pSample);

    if(hr != MF_E_TRANSFORM_NEED_MORE_INPUT && FAILED(hr))
        throw HR_EXCEPTION(hr);
    return (SUCCEEDED(hr) && sample);
}

void transform_h264_encoder::process_output_cb(void*)
{
    std::unique_lock<std::mutex> lock(this->process_output_mutex);

    HRESULT hr = S_OK;
    CComPtr<IMFSample> sample;

    this->process_output(sample);

    if(sample)
    {
        // remove sample tracker from the sample
        CComPtr<IUnknown> obj;
        hr = sample->GetUnknown(media_sample_tracker_guid, __uuidof(IUnknown), (LPVOID*)&obj);
        if(SUCCEEDED(hr))
            // release the buffer so that it is available for reuse
            sample->SetUnknown(media_sample_tracker_guid, nullptr);
        else if(hr != MF_E_ATTRIBUTENOTFOUND)
            CHECK_HR(hr);
        hr = S_OK;

        if(!this->out_sample)
        {
            buffer_pool_h264_frames_t::scoped_lock lock(this->buffer_pool_h264_frames->mutex);
            this->out_sample = this->buffer_pool_h264_frames->acquire_buffer();
            this->out_sample->initialize();
        }

        media_sample_h264_frame frame;
        LONGLONG ts, dur;
        CHECK_HR(hr = sample->GetSampleTime(&ts));
        CHECK_HR(hr = sample->GetSampleDuration(&dur));
        frame.ts = (time_unit)ts;
        frame.dur = (time_unit)dur;
        frame.sample = sample;
        this->out_sample->frames.push_back(frame);
    }

done:
    if(FAILED(hr))
    {
        // TODO: psample is leaked on audio pipeline
        throw HR_EXCEPTION(hr);
    }
}

void transform_h264_encoder::initialize(const control_class_t& ctrl_pipeline,
    const CComPtr<ID3D11Device>& d3d11dev, 
    UINT32 frame_rate_num, UINT32 frame_rate_den,
    UINT32 frame_width, UINT32 frame_height,
    UINT32 avg_bitrate, UINT32 quality_vs_speed,
    const CLSID* clsid,
    bool software)
{
    HRESULT hr = S_OK;

    this->ctrl_pipeline = ctrl_pipeline;
    this->use_system_memory = !d3d11dev || software;
    this->software = software;
    this->frame_rate_num = frame_rate_num;
    this->frame_rate_den = frame_rate_den;
    this->frame_width = frame_width;
    this->frame_height = frame_height;
    this->avg_bitrate = avg_bitrate;
    this->quality_vs_speed = quality_vs_speed;

    CComPtr<IMFAttributes> attributes;
    UINT count = 0;
    UINT activate_index = 0;
    // array must be released with cotaskmemfree
    IMFActivate** activate = NULL;
    MFT_REGISTER_TYPE_INFO info = {MFMediaType_Video, MFVideoFormat_H264};
    UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER;
    if(!software)
        flags |= MFT_ENUM_FLAG_HARDWARE;
    CHECK_HR(hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        flags,
        NULL,
        &info,
        &activate,
        &count));

    // find the requested encoder
    if(clsid)
    {
        bool found = false;
        for(UINT i = 0; i < count; i++)
        {
            static_assert(std::is_same_v<
                decltype(clsid),
                const CLSID*>);

            CLSID clsid2;
            CHECK_HR(hr = activate[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid2));

            if(std::memcmp(clsid, &clsid2, sizeof(CLSID)) == 0)
            {
                found = true;
                activate_index = i;
                break;
            }
        }

        if(!found)
            CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);
    }

    if(!count)
        CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);

    // activate the encoder
    CHECK_HR(hr = 
        activate[activate_index]->ActivateObject(__uuidof(IMFTransform), (void**)&this->encoder));

    // check if the encoder supports d3d11
    CHECK_HR(hr = this->encoder->GetAttributes(&attributes));
    if(!this->use_system_memory)
    {
        UINT32 d3d11_support;
        CHECK_HR(hr = attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_support));
        if(!d3d11_support)
            CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);
    }

    // verify that the encoder is async
    if(!this->software)
    {
        UINT32 async_support;
        CHECK_HR(hr = attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async_support));
        if(!async_support)
            CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);
        // unlock the encoder(must be done for asynchronous transforms)
        CHECK_HR(hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
    }

    // check that the encoder transform has only a fixed number of streams
    DWORD input_stream_count, output_stream_count;
    CHECK_HR(hr = this->encoder->GetStreamCount(&input_stream_count, &output_stream_count));
    if(input_stream_count != 1 || output_stream_count != 1)
        CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);

    hr = this->encoder->GetStreamIDs(
        input_stream_count, &this->input_id, output_stream_count, &this->output_id);
    if(hr == E_NOTIMPL)
        this->input_id = this->output_id = 0;
    else if(FAILED(hr))
        CHECK_HR(hr);

    // associate a dxgidevicemanager with the encoder;
    // this might fail if the adapter backing up the d3d device is
    // incompatible with the mft
    if(!this->use_system_memory)
    {
        CHECK_HR(hr = MFCreateDXGIDeviceManager(&this->reset_token, &this->devmngr));
        CHECK_HR(hr = this->devmngr->ResetDevice(d3d11dev, this->reset_token));
        CHECK_HR(hr = this->encoder->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)this->devmngr.p));
    }

    // set the encoder parameters
    CHECK_HR(hr = this->set_encoder_parameters());

    // set media types for the encoder(output type must be set first)
    CHECK_HR(hr = this->set_output_stream_type());
    CHECK_HR(hr = this->set_input_stream_type());

    // get the buffer requirements for the encoder
    CHECK_HR(hr = this->encoder->GetInputStreamInfo(this->input_id, &this->input_stream_info));
    CHECK_HR(hr = this->encoder->GetOutputStreamInfo(this->output_id, &this->output_stream_info));

    if(this->input_stream_info.dwFlags & MFT_INPUT_STREAM_DOES_NOT_ADDREF)
        CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);

    // get the media event generator interface
    if(!this->software)
    {
        CHECK_HR(hr = this->encoder->QueryInterface(&this->event_generator));
        // register this to the event callback function
        this->events_callback->set_callback(this->shared_from_this<transform_h264_encoder>());
    }

    /*
    amd holds the reference to the submitted buffer
    amd hardware mft supports nv12 and argb32 input types

    intel quicksync only supports nv12 and is not direct3d aware
    microsoft async mft supports only nv12 and is not direct3d aware
    */

    // start the encoder
    {
        std::unique_lock<std::recursive_mutex> lock(*this->context_mutex, std::defer_lock);
        if(!this->use_system_memory)
            lock.lock();
        if(!this->software)
            CHECK_HR(hr = this->event_generator->BeginGetEvent(&this->events_callback->native, NULL));
        CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
        CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));
    }

done:
    // release allocated memory
    if(activate)
    {
        for(UINT i = 0; i < count; i++)
            activate[i]->Release();
        CoTaskMemFree(activate);
    }

    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

media_stream_t transform_h264_encoder::create_stream(media_message_generator_t&& event_generator)
{
    media_stream_message_listener_t stream(
        new stream_h264_encoder(this->shared_from_this<transform_h264_encoder>()));
    stream->register_listener(event_generator);

    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_h264_encoder::stream_h264_encoder(const transform_h264_encoder_t& transform) :
    media_stream_message_listener(transform.get()),
    transform(transform),
    stopping(false)
{
}

void stream_h264_encoder::on_component_start(time_unit t)
{
    if(this->transform->time_shift < 0)
        this->transform->time_shift = t;
}

void stream_h264_encoder::on_component_stop(time_unit)
{
    this->stopping = true;
}

media_stream::result_t stream_h264_encoder::request_sample(const request_packet& rp, const media_stream*)
{
    this->transform->requests.initialize_queue(rp);

    if(!this->transform->session->request_sample(this, rp))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_h264_encoder::process_sample(
    const media_component_args* args_, const request_packet& rp, const media_stream*)
{
    transform_h264_encoder::request_t request;
    request.stream = this;
    request.sample.drain = (rp.flags & FLAG_LAST_PACKET) && this->stopping;
    if(args_)
    {
        request.sample.args =
            std::make_optional(static_cast<const media_component_h264_encoder_args&>(*args_));
        assert_(request.sample.args->is_valid());
    }
    request.sample.already_served = !request.sample.drain &&
        (!request.sample.args || !request.sample.args->has_frames);
    request.rp = rp;
    this->transform->requests.push(request);

    // TODO: the stored request should be served on process_output_cb;
    // the request packet numbering can be reordered; last packet needs to have the last number
    // though
    // requests should be served while the buffer is not full;
    // packet numbers are also reassigned;
    // async processing doesn't really fit into this pipeline design;
    // components should request/pass samples independently from the video_sink

    /*
    TODO:
    add internal source frame buffer for h264 encoder:

    * h264 encoder should add new frames to the raw internal queue and pass requests as null 
    downstream; only if the internal buffer is full should the encoder stall by storing requests; 
    requests are served from the encoded internal queue

    * h264 encoder should keep encoding while the internal buffer is nonempty

    internal buffer might alleviate occasional encoder stalls that cause frame drops

    */

    // pass null requests downstream
    if(request.sample.already_served)
        this->transform->session->give_sample(this, NULL, request.rp);

    /*std::cout << rp.packet_number << std::endl;*/
    // reinitialization seems to work only if the same encoder is reused
    /*static int count = 0;
    if(count == 100)
        this->transform->request_reinitialization(this->transform->ctrl_pipeline);
    count++;*/

    this->transform->serve();

    return OK;
}