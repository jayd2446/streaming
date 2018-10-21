#include "transform_h264_encoder.h"
#include <Mferror.h>
#include <initguid.h>
#include <evr.h>
#include <codecapi.h>
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

class media_buffer_wrapper : public IMFMediaBuffer, IUnknownImpl
{
private:
    media_sample_texture texture;
    D3D11_TEXTURE2D_DESC desc;
public:
    explicit media_buffer_wrapper(const context_mutex_t& context_mutex,
        const media_sample_texture& texture, 
        const CComPtr<IMF2DBuffer>& buffer2d) :
        texture(texture)
    {
        transform_h264_encoder::scoped_lock lock(*context_mutex);

        this->texture.buffer->texture->GetDesc(&this->desc);
        assert_(this->desc.Format == DXGI_FORMAT_NV12);

        HRESULT hr = S_OK;
        DWORD len;

        // the buffer needs to be copied to another buffer, because
        // the context mutex needs to be locked for the whole duration of lock/unlock(=map/unmap);
        // otherwise context corruption will occur even without the debugging layer warning about it

        // it is simply assumed that the texture desc won't be different from what it
        // was when the texture_buffer was firstly allocated
        // (input type for the encoder implies that the desc won't change)
        CHECK_HR(hr = buffer2d->GetContiguousLength(&len));
        if(!this->texture.buffer->texture_buffer)
        {
            this->texture.buffer->texture_buffer_length = len;
            this->texture.buffer->texture_buffer.reset(
                new BYTE[this->texture.buffer->texture_buffer_length]);
        }

        assert_(len == this->texture.buffer->texture_buffer_length);
        CHECK_HR(hr = buffer2d->ContiguousCopyTo(
            this->texture.buffer->texture_buffer.get(),
            this->texture.buffer->texture_buffer_length));

    done:
        if(FAILED(hr))
            throw HR_EXCEPTION(hr);
    }
    virtual ~media_buffer_wrapper()
    {
    }

    ULONG STDMETHODCALLTYPE AddRef() {return IUnknownImpl::AddRef();}
    ULONG STDMETHODCALLTYPE Release() {return IUnknownImpl::Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv)
    {
        if(!ppv)
            return E_POINTER;
        if(riid == __uuidof(IUnknown))
            *ppv = static_cast<IUnknown*>(static_cast<IMFMediaBuffer*>(this));
        else if(riid == __uuidof(IMFMediaBuffer))
            *ppv = static_cast<IMFMediaBuffer*>(this);
        else
        {
            *ppv = NULL;
            return E_NOINTERFACE;
        }

        this->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Lock(BYTE **ppbBuffer, DWORD *pcbMaxLength, DWORD *pcbCurrentLength)
    {
        HRESULT hr = S_OK;

        if(!ppbBuffer)
            CHECK_HR(hr = E_POINTER);
        if(pcbMaxLength)
            CHECK_HR(hr = this->GetMaxLength(pcbMaxLength));
        if(pcbCurrentLength)
            CHECK_HR(hr = this->GetCurrentLength(pcbCurrentLength));

        *ppbBuffer = this->texture.buffer->texture_buffer.get();

    done:
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Unlock()
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentLength(DWORD *pcbCurrentLength)
    {
        HRESULT hr = S_OK;

        if(!pcbCurrentLength)
            CHECK_HR(hr = E_POINTER);

        *pcbCurrentLength = this->texture.buffer->texture_buffer_length;

    done:
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentLength(DWORD /*cbCurrentLength*/)
    {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetMaxLength(DWORD *pcbMaxLength)
    {
        return this->GetCurrentLength(pcbMaxLength);
    }
};

class media_sample_tracker : public IUnknown, IUnknownImpl
{
public:
    media_sample_texture texture;

    explicit media_sample_tracker(const media_sample_texture& texture) : texture(texture) {}
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
    media_source(session),
    encoder_requests(0),
    last_time_stamp(std::numeric_limits<time_unit>::min()),
    last_time_stamp2(std::numeric_limits<time_unit>::min()),
    last_packet(std::numeric_limits<int>::min()),
    context_mutex(context_mutex),
    use_system_memory(false),
    software(false),
    draining(false),
    first_sample(true)
{
    this->events_callback.Attach(new async_callback_t(&transform_h264_encoder::events_cb));
    this->process_output_callback.Attach(
        new async_callback_t(&transform_h264_encoder::process_output_cb));
    this->process_input_callback.Attach(
        new async_callback_t(&transform_h264_encoder::process_input_cb));
}

transform_h264_encoder::~transform_h264_encoder()
{
    HRESULT hr = S_OK;
    CComPtr<IMFShutdown> shutdown;
    if(this->encoder && SUCCEEDED(hr = this->encoder->QueryInterface(&shutdown)))
        hr = shutdown->Shutdown();
}

HRESULT transform_h264_encoder::set_input_stream_type()
{
    HRESULT hr = S_OK;

    CComPtr<IMFMediaType> input_type;
    CHECK_HR(hr = MFCreateMediaType(&input_type));
    CHECK_HR(hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    /*CHECK_HR(hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32));*/
    CHECK_HR(hr = MFSetAttributeRatio(input_type, MF_MT_FRAME_RATE, frame_rate_num, frame_rate_den));
    CHECK_HR(hr = MFSetAttributeSize(input_type, MF_MT_FRAME_SIZE, frame_width, frame_height));
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
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AVG_BITRATE, avg_bitrate));
    CHECK_HR(hr = MFSetAttributeRatio(this->output_type, MF_MT_FRAME_RATE, frame_rate_num, frame_rate_den));
    CHECK_HR(hr = MFSetAttributeSize(this->output_type, MF_MT_FRAME_SIZE, frame_width, frame_height));
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
    v.ullVal = quality_vs_speed;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v));
    v.vt = VT_UI4;
    v.ullVal = avg_bitrate;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v));
    v.vt = VT_UI4;
    v.ullVal = 1;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v));
    /*v.vt = VT_BOOL;
    v.ulVal = VARIANT_FALSE;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVLowLatencyMode, &v));*/
    /*v.vt = VT_UI4;
    v.ulVal = 0;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVEncNumWorkerThreads, &v));*/

done:
    return hr;
}

HRESULT transform_h264_encoder::feed_encoder(const request_t& request)
{
    HRESULT hr = S_OK;

    CComPtr<IMFMediaBuffer> buffer;
    CComPtr<IMFSample> sample;
    CComPtr<IUnknown> sample_tracker;

    // create the input sample buffer
    if(!this->use_system_memory)
    {
        CHECK_HR(hr = MFCreateDXGISurfaceBuffer(
            IID_ID3D11Texture2D,
            request.sample_view.sample.buffer->texture, 0, FALSE, &buffer));
    }
    else
    {
        CComPtr<IMFMediaBuffer> dxgi_buffer;
        CComPtr<IMF2DBuffer> buffer2d;

        CHECK_HR(hr = MFCreateDXGISurfaceBuffer(
            IID_ID3D11Texture2D,
            request.sample_view.sample.buffer->texture, 0, FALSE, &dxgi_buffer));
        CHECK_HR(hr = dxgi_buffer->QueryInterface(&buffer2d));
        buffer.Attach(new media_buffer_wrapper(
            this->context_mutex, request.sample_view.sample, buffer2d));
    }

    const LONGLONG sample_duration = (LONGLONG)
        (SECOND_IN_TIME_UNIT / (double)(frame_rate_num / frame_rate_den));

    // create sample
    CHECK_HR(hr = MFCreateSample(&sample));
    CHECK_HR(hr = sample->AddBuffer(buffer));
    CHECK_HR(hr = sample->SetSampleTime(request.sample_view.sample.timestamp));
    CHECK_HR(hr = sample->SetSampleDuration(sample_duration));
    // the amd encoder probably copies the discontinuity flag to output sample,
    // which might cause problems when the sample is passed to sinkwriter
    if((request.rp.flags & FLAG_DISCONTINUITY) || this->first_sample)
    {
        CHECK_HR(hr = sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE));
        this->first_sample = false;
    }
    /*else
        CHECK_HR(hr = sample->SetUINT32(MFSampleExtension_Discontinuity, FALSE));*/
    // add the tracker attribute to the sample
    sample_tracker.Attach(new media_sample_tracker(request.sample_view.sample));
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
        const HRESULT hr = this->process_input_callback->mf_put_work_item(
            this->shared_from_this<transform_h264_encoder>());
        if(FAILED(hr) && hr != MF_E_SHUTDOWN)
            throw HR_EXCEPTION(hr);
        else if(hr == MF_E_SHUTDOWN)
            return;
    }
    else if(type == METransformHaveOutput)
    {
        const HRESULT hr = this->process_output_callback->mf_put_work_item(
            this->shared_from_this<transform_h264_encoder>());
        if(FAILED(hr) && hr != MF_E_SHUTDOWN)
            throw HR_EXCEPTION(hr);
        else if(hr == MF_E_SHUTDOWN)
            return;
    }
    else if(type == METransformDrainComplete)
    {
        media_buffer_h264_t out_buffer;
        request_t request = this->last_request;
        this->last_request = request_t();
        {
            scoped_lock lock(this->process_output_mutex);
            out_buffer = std::move(this->out_buffer);
        }

        this->process_request(out_buffer, request);
    }
    else if(type == MEError)
    {
        // status has the error code
        throw HR_EXCEPTION(hr);
    }
    else
        assert_(false);

    // set callback for the next event
    CHECK_HR(hr = this->event_generator->BeginGetEvent(&this->events_callback->native, NULL));

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void transform_h264_encoder::processing_cb(void*)
{
    // TODO: software encoder drain
    // encoder needs to be locked so that the samples are submitted in right order
    std::unique_lock<std::recursive_mutex> lock(this->process_mutex);

    HRESULT hr = S_OK;
    
    request_t request;
    while((this->encoder_requests || this->software) && this->requests.pop(request))
    {
        assert_(request.sample_view.sample.buffer->texture);
        const time_unit timestamp = request.sample_view.sample.timestamp;

        if(timestamp <= this->last_time_stamp)
        {
            std::cout << "timestamp error in transform_h264_encoder::processing_cb" << std::endl;
            assert_(false);
        }

    back:
        hr = this->feed_encoder(request);

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

        // event callback will dispatch the last request
        if(!request.sample_view.drain || this->software)
        {
            media_buffer_h264_t out_buffer;
            {
                scoped_lock lock(this->process_output_mutex);
                out_buffer = std::move(this->out_buffer);
            }

            lock.unlock();
            this->process_request(out_buffer, request);
            lock.lock();
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
}

void transform_h264_encoder::process_request(const media_buffer_h264_t& buffer, request_t& request)
{
    HRESULT hr = S_OK;
    media_sample_h264 sample;
    request_packet rp = request.rp;
    const media_stream* stream = request.stream;
    LONGLONG duration = -1;

    assert_(!buffer || !buffer->samples.empty());

    if(buffer)
    {
        CHECK_HR(hr = buffer->samples[0]->GetSampleTime(&sample.timestamp));
        CHECK_HR(hr = buffer->samples[0]->GetSampleDuration(&duration));
    }
    else
        sample.timestamp = std::numeric_limits<LONGLONG>::min();

    this->last_packet = rp.packet_number;

    sample.buffer = buffer;
    sample.software = this->software;

    // reset the sample so that the contained buffer can be reused
    request.sample_view.sample = media_sample_texture();

    this->session->give_sample(stream, sample, rp, false);

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
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
        CHECK_HR(hr = MFCreateSample(&sample));
        CHECK_HR(hr = MFCreateMemoryBuffer(this->output_stream_info.cbSize, &buffer));
        CHECK_HR(hr = sample->AddBuffer(buffer));
        output.pSample = sample;
    }

    // TODO: use preallocated memory for the mft

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
    std::unique_lock<std::recursive_mutex> lock(this->process_output_mutex);

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
            static_cast<media_sample_tracker*>(obj.p)->texture.buffer.reset();
        else if(hr != MF_E_ATTRIBUTENOTFOUND)
            CHECK_HR(hr);
        hr = S_OK;

        if(!this->out_buffer)
            this->out_buffer.reset(new media_buffer_h264);
        this->out_buffer->samples.push_back(sample);
    }

done:
    if(FAILED(hr))
    {
        // TODO: psample is leaked on audio pipeline
        throw HR_EXCEPTION(hr);
    }
}

void transform_h264_encoder::process_input_cb(void*)
{
    this->processing_cb(NULL);
}

void transform_h264_encoder::initialize(const control_class_t& ctrl_pipeline,
    const CComPtr<ID3D11Device>& d3d11dev, bool software)
{
    HRESULT hr = S_OK;

    this->ctrl_pipeline = ctrl_pipeline;
    this->use_system_memory = !d3d11dev || software;
    this->software = software;

    CComPtr<IMFAttributes> attributes;
    UINT count = 0;
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

    if(!count)
        CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);

    // activate the first encoder
    CHECK_HR(hr = activate[0]->ActivateObject(__uuidof(IMFTransform), (void**)&this->encoder));

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

media_stream_t transform_h264_encoder::create_stream(presentation_clock_t&& clock)
{
    media_stream_clock_sink_t stream(
        new stream_h264_encoder(this->shared_from_this<transform_h264_encoder>()));
    stream->register_sink(clock);

    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_h264_encoder::stream_h264_encoder(const transform_h264_encoder_t& transform) :
    media_stream_clock_sink(transform.get()),
    transform(transform),
    drain_point(std::numeric_limits<time_unit>::min())
{
}

void stream_h264_encoder::on_component_start(time_unit)
{
}

void stream_h264_encoder::on_component_stop(time_unit t)
{
    this->drain_point = t;
}

media_stream::result_t stream_h264_encoder::request_sample(request_packet& rp, const media_stream*)
{
    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_h264_encoder::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream*)
{
    transform_h264_encoder::request_t request;
    request.stream = this;
    request.sample_view.drain = (rp.request_time == this->drain_point);
    request.sample_view.sample = reinterpret_cast<const media_sample_texture&>(sample_view);
    request.rp = rp;

    this->transform->requests.push(request);

    /*std::cout << rp.packet_number << std::endl;*/

    // reinitialization seems to work only if the same encoder is reused
    /*static int count = 0;
    if(count == 100)
        this->transform->request_reinitialization(this->transform->ctrl_pipeline);
    count++;*/

    this->transform->processing_cb(NULL);
    return OK;
}