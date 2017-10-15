#include "transform_h264_encoder.h"
#include <Mferror.h>
#include <evr.h>
#include <iostream>
#include <cassert>

#pragma comment(lib, "Evr.lib")
#pragma comment(lib, "dxguid.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

transform_h264_encoder::transform_h264_encoder(const media_session_t& session) :
    media_source(session),
    last_packet_number(-1),
    encoder_requests(0)
{
    this->processing_callback.Attach(new async_callback_t(&transform_h264_encoder::processing_cb));
    this->events_callback.Attach(new async_callback_t(&transform_h264_encoder::events_cb));
    this->process_output_callback.Attach(new async_callback_t(&transform_h264_encoder::process_output_cb));
    this->process_input_callback.Attach(new async_callback_t(&transform_h264_encoder::process_input_cb));
}

HRESULT transform_h264_encoder::set_input_stream_type()
{
    HRESULT hr = S_OK;

    // TODO: input type should be get from the color converter transform

    CComPtr<IMFMediaType> input_type;
    CHECK_HR(hr = MFCreateMediaType(&input_type));
    CHECK_HR(hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    CHECK_HR(hr = MFSetAttributeRatio(input_type, MF_MT_FRAME_RATE, 60, 1));
    CHECK_HR(hr = MFSetAttributeSize(input_type, MF_MT_FRAME_SIZE, 1920, 1080));
    CHECK_HR(hr = input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK_HR(hr = input_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    CHECK_HR(hr = MFSetAttributeRatio(input_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

    CHECK_HR(hr = this->encoder->SetInputType(0, input_type, 0));
done:
    return hr;
}

HRESULT transform_h264_encoder::set_output_stream_type()
{
    /*HRESULT hr = S_OK;

    CComPtr<IMFMediaType> output_type;
    for(DWORD index = 0;; index++)
    {
        CComPtr<IMFMediaType> type;
        GUID subtype;
        CHECK_HR(hr = this->encoder->GetOutputAvailableType(0, index, &type));
        CHECK_HR(hr = type->GetGUID(MF_MT_SUBTYPE, &subtype));
        if(IsEqualGUID(subtype, MFVideoFormat_H264))
        {
            output_type = type;
            break;
        }
    }*/

    HRESULT hr = S_OK;
    CHECK_HR(hr = MFCreateMediaType(&this->output_type));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AVG_BITRATE, 6000*1000));
    CHECK_HR(hr = MFSetAttributeRatio(this->output_type, MF_MT_FRAME_RATE, 60, 1));
    CHECK_HR(hr = MFSetAttributeSize(this->output_type, MF_MT_FRAME_SIZE, 1920, 1080));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    CHECK_HR(hr = MFSetAttributeRatio(this->output_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

    CHECK_HR(hr = this->encoder->SetOutputType(0, this->output_type, 0));
done:
    return hr;
}

void transform_h264_encoder::events_cb(void* unk)
{
    // TODO: the events might not reach every stream

    // TODO: upradeable lock for stream events callback
    // TODO: this should be somewhat synchronized because endgetevent shouldn't be called twice
    IMFAsyncResult* result = (IMFAsyncResult*)unk;
    HRESULT hr = S_OK;
    CComPtr<IMFMediaEvent> media_event;
    // get the event from the event queue
    CHECK_HR(hr = this->event_generator->EndGetEvent(result, &media_event));

    // set callback for the next event
    CHECK_HR(hr = this->event_generator->BeginGetEvent(&this->events_callback->native, NULL));

    // process the event
    MediaEventType type = MEUnknown;
    HRESULT status = S_OK;
    CHECK_HR(hr = media_event->GetType(&type));
    CHECK_HR(hr = media_event->GetStatus(&status));

    if(type == METransformNeedInput)
    {
        UINT32 stream_id;
        CHECK_HR(hr = media_event->GetUINT32(MF_EVENT_MFT_INPUT_STREAM_ID, &stream_id));
        const HRESULT hr = this->process_input_callback->mf_put_work_item(
            this->shared_from_this<transform_h264_encoder>(),
            MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
        if(FAILED(hr) && hr != MF_E_SHUTDOWN)
            throw std::exception();
        else if(hr == MF_E_SHUTDOWN)
            return;
    }
    else if(type == METransformHaveOutput)
    {
        const HRESULT hr = this->process_output_callback->mf_put_work_item(
            this->shared_from_this<transform_h264_encoder>(),
            MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
        if(FAILED(hr) && hr != MF_E_SHUTDOWN)
            throw std::exception();
        else if(hr == MF_E_SHUTDOWN)
            return;
    }
    else
        assert(false);

done:
    if(FAILED(hr))
        throw std::exception();
}

void transform_h264_encoder::processing_cb(void*)
{
    HRESULT hr = S_OK;
    while(this->encoder_requests)
    {
        // encoder needs to be locked so that the samples are submitted in right order
        scoped_lock lock(this->encoder_mutex);
        packet p;
        {
            scoped_lock lock(this->samples_mutex);
            scoped_lock lock2(this->processed_samples_mutex);

            if(!this->encoder_requests || this->samples.empty())
                return;

            // pull the next sample from the queue
            auto first_item = this->samples.begin();
            if(this->last_packet_number != -1 && first_item->first != (this->last_packet_number + 1))
                return;

            this->encoder_requests--;
            p = first_item->second;
            this->samples.erase(first_item);

            // push the sample to the processed samples queue
            this->processed_samples.push(p);
        }

        // submit the sample to the encoder
        CComPtr<IMFMediaBuffer> buffer;
        CComPtr<IMF2DBuffer> buffer2d;
        CComPtr<IMFSample> sample;
        DWORD len;

        // (cant lock the texture because the unlocking can happen in another thread)
        CHECK_HR(hr = MFCreateDXGISurfaceBuffer(
            IID_ID3D11Texture2D,
            p.sample_view->get_sample<media_sample_texture>()->texture, 0, FALSE, &buffer));
        CHECK_HR(hr = buffer->QueryInterface(&buffer2d));
        CHECK_HR(hr = buffer2d->GetContiguousLength(&len));
        CHECK_HR(hr = buffer->SetCurrentLength(len));
        CHECK_HR(hr = MFCreateVideoSampleFromSurface(NULL, &sample));
        CHECK_HR(hr = sample->AddBuffer(buffer));
        CHECK_HR(hr = this->encoder->ProcessInput(0, sample, 0));

        // TODO: maybe set sample time aswell
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

void transform_h264_encoder::process_output_cb(void*)
{
    HRESULT hr = S_OK;

    media_sample_memorybuffer_t sample(new media_sample_memorybuffer);
    media_sample_view_t sample_view(new media_sample_view(sample));
    packet p;

    const DWORD mft_provides_samples =
        this->output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;

    MFT_OUTPUT_DATA_BUFFER output;
    DWORD status = 0;
    output.dwStreamID = 0;
    if(mft_provides_samples)
        output.pSample = NULL;
    else
        throw std::exception();
    output.dwStatus = 0;
    output.pEvents = NULL;

    CComPtr<IMFMediaBuffer> buffer;
    CHECK_HR(hr = this->encoder->ProcessOutput(0, 1, &output, &status));
    /*CHECK_HR(hr = output.pSample->ConvertToContiguousBuffer(&buffer));*/
    //// release the mft allocated buffer
    //output.pSample->Release();

    // TODO: frame drops must be handled in encoder

    {
        scoped_lock lock(this->processed_samples_mutex);
        p = this->processed_samples.front();
        this->processed_samples.pop();
    }

    sample->buffer = buffer;
    sample->timestamp = p.sample_view->get_sample()->timestamp;
    sample->sample.Attach(output.pSample);
    CHECK_HR(hr = sample->sample->SetSampleTime(p.rp.request_time));

    this->session->give_sample(p.stream, sample_view, p.rp, false);
done:
    if(FAILED(hr))
        throw std::exception();
}

void transform_h264_encoder::process_input_cb(void*)
{
    this->encoder_requests++;
    this->processing_cb(NULL);
}

HRESULT transform_h264_encoder::initialize(const CComPtr<ID3D11Device>& d3d11dev)
{
    HRESULT hr = S_OK;

    this->d3d11dev = d3d11dev;
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
    CHECK_HR(hr = activate[0]->ActivateObject(__uuidof(IMFTransform), (void**)&this->encoder));

    // check if the encoder supports d3d11
    UINT32 d3d11_support;
    CHECK_HR(hr = this->encoder->GetAttributes(&attributes));
    CHECK_HR(hr = attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_support));
    if(!d3d11_support)
        CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);

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

    DWORD input_id, output_id;
    hr = this->encoder->GetStreamIDs(
        input_stream_count, &input_id, output_stream_count, &output_id);
    if(hr != E_NOTIMPL)
        CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);

    // set media types for the encoder(output type must be set first)
    CHECK_HR(hr = this->set_output_stream_type());
    CHECK_HR(hr = this->set_input_stream_type());
    /*hr = this->set_input_stream_type();
    if(hr == MF_E_TRANSFORM_TYPE_NOT_SET)
    {
        CHECK_HR(hr = this->set_output_stream_type());
        CHECK_HR(hr = this->set_input_stream_type());
    }
    else if(hr == S_OK)
        CHECK_HR(hr = this->set_output_stream_type())
    else
        goto done;*/

    // get the buffer requirements for the encoder
    CHECK_HR(hr = this->encoder->GetInputStreamInfo(0, &this->input_stream_info));
    CHECK_HR(hr = this->encoder->GetOutputStreamInfo(0, &this->output_stream_info));

    // associate a dxgidevicemanager with the encoder
    CHECK_HR(hr = MFCreateDXGIDeviceManager(&this->reset_token, &this->devmngr));
    CHECK_HR(hr = this->devmngr->ResetDevice(this->d3d11dev, this->reset_token));
    CHECK_HR(hr = this->encoder->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)this->devmngr.p));

    // get the media event generator interface
    CHECK_HR(hr = this->encoder->QueryInterface(&this->event_generator));
    // register this to the event callback function
    this->events_callback->set_callback(this->shared_from_this<transform_h264_encoder>());

    /*
    amd hardware mft supports nv12 and argb32 input types

    intel quicksync only supports nv12 and is not direct3d aware
    microsoft async mft supports only nv12 and is not direct3d aware
    */

    // start the encoder
    CHECK_HR(hr = this->event_generator->BeginGetEvent(&this->events_callback->native, NULL));
    CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

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

    return hr;
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

media_stream::result_t stream_h264_encoder::request_sample(request_packet& rp)
{
    if(!this->transform->session->request_sample(this, rp, false))
        return FATAL_ERROR;
    return OK;
}

media_stream::result_t stream_h264_encoder::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp)
{
    if(!sample_view->get_sample<media_sample_texture>()->texture)
        if(!this->transform->session->give_sample(this, sample_view, rp, false))
            return FATAL_ERROR;
        else
            return OK;

    {
        scoped_lock lock(this->transform->samples_mutex);
        transform_h264_encoder::packet p;
        p.rp = rp;
        p.sample_view = sample_view;
        p.stream = this;
        this->transform->samples[rp.packet_number] = p;
    }

    this->transform->processing_cb(NULL);
    return OK;
}