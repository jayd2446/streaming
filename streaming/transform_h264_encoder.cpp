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
//        throw std::exception();
//}
#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#undef max
#undef min

// TODO: the media buffer wrapper class should be used to track the lifetime of the texture
// object so that it is unlocked as soon as possible
// (if that is possible);
// actually, imftrackedsample should be used for tracking the locking
class media_buffer_wrapper : public IMFMediaBuffer, IUnknownImpl
{
private:
    media_sample_texture texture;
    D3D11_TEXTURE2D_DESC desc;
    CComPtr<IMF2DBuffer> buffer2d;
    context_mutex_t context_mutex;
    BYTE* buffer;
    DWORD len;
public:
    explicit media_buffer_wrapper(const context_mutex_t& context_mutex,
        const media_sample_texture& texture, 
        const CComPtr<IMF2DBuffer>& buffer2d) :
        context_mutex(context_mutex), texture(texture), buffer2d(buffer2d),
        buffer(NULL)
    {
        transform_h264_encoder::scoped_lock lock(*this->context_mutex);

        this->texture.buffer->texture->GetDesc(&this->desc);

        assert_(this->desc.Format == DXGI_FORMAT_NV12);

        HRESULT hr = S_OK;

        // the buffer needs to be copied to another buffer, because
        // the context mutex needs to be locked for the whole duration of lock/unlock(=map/unmap);
        // otherwise context corruption will occur even without the debugging layer warning about it

        // it is simply assumed that the texture desc won't be different from what it
        // was when the texture_buffer was firstly allocated
        // (input type for the encoder implies that the desc won't change)
        CHECK_HR(hr = this->buffer2d->GetContiguousLength(&this->len));
        if(!this->texture.buffer->texture_buffer)
        {
            this->texture.buffer->texture_buffer_length = this->len;
            this->texture.buffer->texture_buffer.reset(
                new BYTE[this->texture.buffer->texture_buffer_length]);
        }

        assert_(this->len == this->texture.buffer->texture_buffer_length);
        CHECK_HR(hr = this->buffer2d->ContiguousCopyTo(
            this->texture.buffer->texture_buffer.get(),
            this->texture.buffer->texture_buffer_length));

        this->buffer = this->texture.buffer->texture_buffer.get();
        this->buffer2d = NULL;

    done:
        if(FAILED(hr))
            throw std::exception();
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

        *ppbBuffer = this->buffer;

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

        *pcbCurrentLength = this->len;

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

class media_sample_wrapper : public IMFSample
{
private:
    media_sample_texture texture;
public:

};

transform_h264_encoder::transform_h264_encoder(const media_session_t& session, 
    context_mutex_t context_mutex) :
    media_source(session),
    encoder_requests(0),
    last_time_stamp(std::numeric_limits<time_unit>::min()),
    last_time_stamp2(std::numeric_limits<time_unit>::min()),
    last_packet(std::numeric_limits<int>::min()),
    context_mutex(context_mutex),
    use_system_memory(false)
{
    this->events_callback.Attach(new async_callback_t(&transform_h264_encoder::events_cb));
    this->process_output_callback.Attach(
        new async_callback_t(&transform_h264_encoder::process_output_cb));
    this->process_input_callback.Attach(
        new async_callback_t(&transform_h264_encoder::process_input_cb));
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
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
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
    v.vt = VT_BOOL;
    v.ulVal = VARIANT_FALSE;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVLowLatencyMode, &v));
    /*v.vt = VT_UI4;
    v.ulVal = 1;
    CHECK_HR(hr = codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v));*/

done:
    return hr;
}

HRESULT transform_h264_encoder::feed_encoder(const request_t& request)
{
    HRESULT hr = S_OK;

    CComPtr<IMFMediaBuffer> buffer;
    CComPtr<IMFSample> sample;

    // submit the sample to the encoder
    if(!this->use_system_memory)
    {
        CHECK_HR(hr = MFCreateDXGISurfaceBuffer(
            IID_ID3D11Texture2D,
            request.sample_view.buffer->texture, 0, FALSE, &buffer));
    }
    else
    {
        CComPtr<IMFMediaBuffer> dxgi_buffer;
        CComPtr<IMF2DBuffer> buffer2d;

        CHECK_HR(hr = MFCreateDXGISurfaceBuffer(
            IID_ID3D11Texture2D,
            request.sample_view.buffer->texture, 0, FALSE, &dxgi_buffer));
        CHECK_HR(hr = dxgi_buffer->QueryInterface(&buffer2d));
        buffer.Attach(new media_buffer_wrapper(
            this->context_mutex, request.sample_view, buffer2d));
    }

    CHECK_HR(hr = MFCreateSample(&sample));
    CHECK_HR(hr = sample->AddBuffer(buffer));
    CHECK_HR(hr = sample->SetSampleTime(request.sample_view.timestamp));
    {
        std::unique_lock<std::recursive_mutex> lock(*this->context_mutex, std::defer_lock);
        if(!this->use_system_memory)
            lock.lock();
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
            throw std::exception();
        else if(hr == MF_E_SHUTDOWN)
            return;
    }
    else if(type == METransformHaveOutput)
    {
        const HRESULT hr = this->process_output_callback->mf_put_work_item(
            this->shared_from_this<transform_h264_encoder>());
        if(FAILED(hr) && hr != MF_E_SHUTDOWN)
            throw std::exception();
        else if(hr == MF_E_SHUTDOWN)
            return;
    }
    else if(type == MEError)
    {
        // status has the error code
        throw std::exception();
    }
    else
        assert_(false);

    // set callback for the next event
    CHECK_HR(hr = this->event_generator->BeginGetEvent(&this->events_callback->native, NULL));

done:
    if(FAILED(hr))
        throw std::exception();
}

void transform_h264_encoder::processing_cb(void*)
{
    // encoder needs to be locked so that the samples are submitted in right order
    scoped_lock lock(this->process_mutex);

    HRESULT hr = S_OK;
    
    request_t request;
    while(this->encoder_requests && this->requests.pop(request))
    {
        assert_(request.sample_view.buffer->texture);

        {
            scoped_lock lock(this->processed_samples_mutex);
            this->processed_samples.push(request);
        }
        this->encoder_requests--;

        const time_unit timestamp = request.sample_view.timestamp;

#ifdef _DEBUG
        if(timestamp <= this->last_time_stamp)
            DebugBreak();
        this->last_time_stamp = timestamp;
#endif

        CHECK_HR(hr = this->feed_encoder(request));
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

void transform_h264_encoder::process_output_cb(void*)
{
    HRESULT hr = S_OK;
    media_sample_h264 sample_view;
    request_packet rp;
    const media_stream* stream;
    constexpr LONGLONG invalid_timestamp = std::numeric_limits<LONGLONG>::min();
    LONGLONG timestamp = invalid_timestamp;

    const DWORD mft_provides_samples =
        this->output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;

    MFT_OUTPUT_DATA_BUFFER output;
    DWORD status = 0;
    output.dwStreamID = this->output_id;
    if(mft_provides_samples)
        output.pSample = NULL;
    else
        throw std::exception();
    output.dwStatus = 0;
    output.pEvents = NULL;

    {
        scoped_lock lock(this->process_output_mutex);

        {
            // TODO: decide if context mutex is really needed here
            std::unique_lock<std::recursive_mutex> lock(*this->context_mutex, std::defer_lock);
            if(!this->use_system_memory)
                lock.lock();
            hr = this->encoder->ProcessOutput(0, 1, &output, &status);
        }
        // the output stream type will be the exact same one, but for some reason
        // intel mft requires resetting the output type
        if(FAILED(hr) && output.dwStatus == MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE)
            CHECK_HR(hr = this->set_output_stream_type())
        else if(SUCCEEDED(hr))
            CHECK_HR(hr = output.pSample->GetSampleTime(&timestamp))
        else
            CHECK_HR(hr)

        {
            scoped_lock lock(this->processed_samples_mutex);
            request_t request = this->processed_samples.front();
            this->processed_samples.pop();

            // intel mft sets its own timestamps, so the assert won't work anymore
            /*assert_(timestamp == request.sample_view.timestamp || timestamp == invalid_timestamp);*/

            rp = request.rp;
            stream = request.stream;
            sample_view.timestamp = timestamp;

#ifdef _DEBUG
            if(output.pSample)
            {
                if(timestamp <= this->last_time_stamp2)
                    DebugBreak();
                this->last_time_stamp2 = timestamp;
            }
            if(rp.packet_number <= this->last_packet)
                DebugBreak();
            this->last_packet = rp.packet_number;
#endif
        }
    }

    if(output.pSample)
    {
        sample_view.buffer.reset(new media_buffer_h264);
        sample_view.buffer->sample.Attach(output.pSample);
    }
    this->session->give_sample(stream, sample_view, rp, false);

done:
    if(FAILED(hr))
        throw std::exception();
}

void transform_h264_encoder::process_input_cb(void*)
{
    this->processing_cb(NULL);
}

void transform_h264_encoder::initialize(const CComPtr<ID3D11Device>& d3d11dev)
{
    HRESULT hr = S_OK;

    this->use_system_memory = !d3d11dev;

    CComPtr<IMFAttributes> attributes;
    UINT count = 0;
    // array must be released with cotaskmemfree
    IMFActivate** activate = NULL;
    MFT_REGISTER_TYPE_INFO info = {MFMediaType_Video, MFVideoFormat_H264};
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
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
    CHECK_HR(hr = activate[1]->ActivateObject(__uuidof(IMFTransform), (void**)&this->encoder));

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
    UINT32 async_support;
    CHECK_HR(hr = attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async_support));
    if(!async_support)
        CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);

    // unlock the encoder(must be done for asynchronous transforms)
    CHECK_HR(hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));

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

    // get the media event generator interface
    CHECK_HR(hr = this->encoder->QueryInterface(&this->event_generator));
    // register this to the event callback function
    this->events_callback->set_callback(this->shared_from_this<transform_h264_encoder>());

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
        throw std::exception();
}

media_stream_t transform_h264_encoder::create_stream()
{
    return media_stream_t(
        new stream_h264_encoder(this->shared_from_this<transform_h264_encoder>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_h264_encoder::stream_h264_encoder(const transform_h264_encoder_t& transform) :
    transform(transform)
{
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
    request.sample_view = reinterpret_cast<const media_sample_texture&>(sample_view);
    request.rp = rp;

    this->transform->requests.push(request);

    /*std::cout << rp.packet_number << std::endl;*/

    this->transform->processing_cb(NULL);
    return OK;
}