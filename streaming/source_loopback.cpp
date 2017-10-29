#include "source_loopback.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <Mferror.h>
#include <cassert>
#include <iostream>
#include <cmath>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000

template<typename T>
void copy_aligned(BYTE* to, const BYTE* from)
{
    *((T*)to) = *((const T*)from);
}

source_loopback::source_loopback(const media_session_t& session) : 
    media_source(session), mf_alignment(-1)
{
    this->started = false;
    this->process_callback.Attach(new async_callback_t(&source_loopback::process_cb));
}

source_loopback::~source_loopback()
{
}

HRESULT source_loopback::copy_aligned(BYTE* to, const BYTE* from) const
{
    switch(this->mf_alignment)
    {
    case MF_4_BYTE_ALIGNMENT:
        ::copy_aligned<uint32_t>(to, from);
        break;
    case MF_8_BYTE_ALIGNMENT:
        ::copy_aligned<uint64_t>(to, from);
        break;
    default:
        return E_FAIL;
    }
    return S_OK;
}

HRESULT source_loopback::add_event_to_wait_queue()
{
    HRESULT hr = S_OK;

    CComPtr<IMFAsyncResult> asyncresult;
    CHECK_HR(hr = MFCreateAsyncResult(NULL, &this->process_callback->native, NULL, &asyncresult));
    // use the priority of 1 for audio
    CHECK_HR(hr = this->process_callback->mf_put_waiting_work_item(
        this->shared_from_this<source_loopback>(),
        this->process_event, 1, asyncresult, &this->callback_key));

done:
    return hr;
}

HRESULT source_loopback::create_waveformat_type(WAVEFORMATEX* format)
{
    HRESULT hr = S_OK;

    UINT32 size = sizeof(WAVEFORMATEX);
    // (WAVEFORMATEX*)format.blob.pBlobData, format.blob.cbSize

    //// coerce int-16 wave format
    //switch(format->wFormatTag)
    //{
    //case WAVE_FORMAT_IEEE_FLOAT:
    //    DebugBreak();
    //    break;
    //case WAVE_FORMAT_EXTENSIBLE:
    //    {
    //        PWAVEFORMATEXTENSIBLE pex = (PWAVEFORMATEXTENSIBLE)format;
    //        if(IsEqualGUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, pex->SubFormat))
    //        {
    //            pex->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    //            pex->Samples.wValidBitsPerSample = 16;
    //            format->wBitsPerSample = 16;
    //            format->nBlockAlign = format->nChannels * format->wBitsPerSample / 8;
    //            format->nAvgBytesPerSec = format->nBlockAlign * format->nSamplesPerSec;
    //        }
    //    }
    //    break;
    //case WAVE_FORMAT_PCM:
    //    DebugBreak();
    //    break;
    //default:
    //    throw std::exception();
    //}
    if(format->wFormatTag != WAVE_FORMAT_PCM)
        size += format->cbSize;

    CHECK_HR(hr = MFCreateMediaType(&this->waveformat_type));
    CHECK_HR(hr = MFInitMediaTypeFromWaveFormatEx(this->waveformat_type, format, size));

done:
    return hr;
}

void source_loopback::process_cb(void*)
{
    ResetEvent(this->process_event);

    UINT32 nextpacketsize = 0;
    HRESULT hr = S_OK;
    while(SUCCEEDED(hr))
    {
        BYTE* data;
        // 1 frame contains samples for every audio channel
        UINT32 frames = 0;
        bool releasebuffer = false;
        DWORD flags;
        // the timestamp of the first sample when it was scheduled to play through speakers
        // in 100 nanosecond units
        UINT64 first_sample_timestamp;
        CComPtr<IMFSample> sample, sample2;
        CComPtr<IMFMediaBuffer> buffer, buffer2;
        presentation_clock_t clock;

        CHECK_HR(hr = this->audio_capture_client->GetNextPacketSize(&nextpacketsize));
        if(!nextpacketsize)
            break;

        if(!this->session->get_current_clock(clock))
            return;

        CHECK_HR(hr = MFCreateSample(&sample));
        CHECK_HR(hr = MFCreateSample(&sample2));

        /*
        Clients should avoid excessive delays between the GetBuffer call
        that acquires a packet and the ReleaseBuffer call that releases the packet.
        The implementation of the audio engine assumes that the GetBuffer call and
        the corresponding ReleaseBuffer call occur within the same buffer-processing period.
        Clients that delay releasing a packet for more than one period risk losing sample data.
        */

        // the data is assumed to be in pcm format
        CHECK_HR(hr = this->audio_capture_client->GetBuffer(
            &data, &frames, &flags, NULL, &first_sample_timestamp));
        releasebuffer = true;

        // the first event has the data discontinuity flag set
        if(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
        {
            std::cerr << "DATA DISCONTINUITY" << std::endl;
        }
        if(flags & AUDCLNT_BUFFERFLAGS_SILENT);
        if(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
        {
            std::cerr << "TIMESTAMP ERROR" << std::endl;
        }
        else
            /*std::cerr << "OK" << std::endl*/;

#undef max
#undef min

        static double incr = 1.0;
        // copy pcm samples to the audio sample
        BYTE* buffer_data;
        CHECK_HR(hr = MFCreateMemoryBuffer(frames * 4, &buffer));
        CHECK_HR(hr = buffer->Lock(&buffer_data, NULL, NULL));
        //for(UINT32 i = 0; i < (frames * this->channels); i++)
        //{
        //    /*incr++;*/
        //    ((int16_t*)buffer_data)[i] = sin(incr) * ((double)std::numeric_limits<int16_t>::max() / 2.0);

        //    if(i % this->channels)
        //        incr += 0.02;
        //}

        // Floating-point PCM samples (32- or 64-bit in size) are zero-centred 
        // and varies in the interval [-1.0, 1.0], thus signed values.
        for(UINT32 i = 0; i < (frames * this->channels); i++)
        {
            double sample = ((float*)data)[i];
            int32_t sample_converted = (int32_t)(sample * std::numeric_limits<int16_t>::max());
            if(sample_converted > std::numeric_limits<int16_t>::max())
                sample_converted = std::numeric_limits<int16_t>::max();
            else if(sample_converted < std::numeric_limits<int16_t>::min())
                sample_converted = std::numeric_limits<int16_t>::min();

            ((int16_t*)buffer_data)[i] = (int16_t)sample_converted;
        }
        CHECK_HR(hr = buffer->Unlock());
        CHECK_HR(hr = buffer->SetCurrentLength(frames * 4));

        CHECK_HR(hr = sample->AddBuffer(buffer));

        // set sample flags
        // TODO: sample timestamp should be adjusted to time source
        // casting to 64 bit number needed so that the duration calculation don't overflow
        static LONGLONG sample_dur = 0;
        CHECK_HR(hr = sample->SetSampleDuration(
            SECOND_IN_TIME_UNIT * 
            (UINT64)frames / (UINT64)this->samples_per_second));
        CHECK_HR(hr = sample->SetSampleTime(
            clock->get_time_source()->system_time_to_time_source(first_sample_timestamp)));

        sample_dur += SECOND_IN_TIME_UNIT * (UINT64)frames / (UINT64)this->samples_per_second;

        // add the sample to the queue
        {
            scoped_lock lock(this->samples_mutex);
            this->samples.push_back(sample);
        }

    done:
        if(releasebuffer)
            this->audio_capture_client->ReleaseBuffer(frames);
    }

    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();

    this->add_event_to_wait_queue();
}

HRESULT source_loopback::initialize()
{
    HRESULT hr = S_OK;

    CComPtr<IMMDeviceEnumerator> enumerator;
    CComPtr<IMMDevice> device;
    CComPtr<IPropertyStore> device_props;
    PROPVARIANT device_format;
    WAVEFORMATEX* engine_format = NULL;
    PropVariantInit(&device_format);

    CHECK_HR(hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator));
    CHECK_HR(hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));

    // get the device's waveformat that is captured by the loopback capturing
    CHECK_HR(hr = device->OpenPropertyStore(STGM_READ, &device_props));
    CHECK_HR(hr = device_props->GetValue(PKEY_AudioEngine_DeviceFormat, &device_format));

    CHECK_HR(hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&this->audio_client));
    CHECK_HR(hr = this->audio_client->GetMixFormat(&engine_format));

    /*switch(this->waveformat->nBlockAlign)
    {
    case 4:
        this->mf_alignment = MF_4_BYTE_ALIGNMENT;
        break;
    case 8:
        this->mf_alignment = MF_8_BYTE_ALIGNMENT;
        break;
    default:
        CHECK_HR(hr = E_FAIL);
    }*/

    CHECK_HR(hr = this->audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 
        0, 0, engine_format, NULL));

    CHECK_HR(hr = this->audio_client->GetBufferSize(&this->samples_size));

    CHECK_HR(hr = this->audio_client->GetService(
        __uuidof(IAudioCaptureClient), (void**)&this->audio_capture_client));

    /*
    In Windows 8, the first use of IAudioClient to access the audio device should be
    on the STA thread. Calls from an MTA thread may result in undefined behavior.
    */

    // create waveformat mediatype
    CHECK_HR(hr = this->create_waveformat_type((WAVEFORMATEX*)engine_format));

    CHECK_HR(hr = this->waveformat_type->GetUINT32(
        MF_MT_AUDIO_NUM_CHANNELS, &this->channels));

    // get samples per second
    CHECK_HR(hr = 
        this->waveformat_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &this->samples_per_second));

    // set block align
    CHECK_HR(hr = this->waveformat_type->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &this->block_align));
    switch(this->block_align)
    {
    case 4:
        this->mf_alignment = MF_4_BYTE_ALIGNMENT;
        break;
    case 8:
        this->mf_alignment = MF_8_BYTE_ALIGNMENT;
        break;
    default:
        CHECK_HR(hr = E_FAIL);
    }

    // create manual reset event handle
    this->process_event.Attach(CreateEvent(
        NULL, TRUE, FALSE, NULL));
    if(!this->process_event)
        CHECK_HR(hr = E_FAIL);

    CHECK_HR(hr = this->audio_client->SetEventHandle(this->process_event));

    // set the event to mf queue
    CHECK_HR(hr = this->add_event_to_wait_queue());

done:
    if(device_format.blob.pBlobData)
        PropVariantClear(&device_format);
    if(engine_format)
        CoTaskMemFree(engine_format);
    if(FAILED(hr))
        throw std::exception();

    return hr;
}

HRESULT source_loopback::start()
{
    HRESULT hr = S_OK;
    bool started = false;

    this->started.compare_exchange_strong(started, true);
    if(!started)
        CHECK_HR(hr = this->audio_client->Start());

done:
    return hr;
}

media_stream_t source_loopback::create_stream(presentation_clock_t& clock)
{
    stream_loopback_t stream(new stream_loopback(this->shared_from_this<source_loopback>()));
    stream->register_sink(clock);

    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_loopback::stream_loopback(const source_loopback_t& source) : 
    source(source),
    sample(new media_sample)
{
    this->process_callback.Attach(new async_callback_t(&stream_loopback::process_cb));
}

bool stream_loopback::on_clock_start(time_unit t)
{
    HRESULT hr = S_OK;
    if(FAILED(hr = this->source->start()))
        throw std::exception();
    return true;
}

void stream_loopback::on_clock_stop(time_unit t)
{
}

void stream_loopback::process_cb(void*)
{
    media_buffer_samples_t samples_buffer;
    source_loopback::request_t request;

    std::unique_lock<std::recursive_mutex> lock(this->source->samples_mutex);
    while(this->source->requests.pop(request))
    {
        if(this->source->samples.empty())
        {
            // pass a null sample to downstream
            lock.unlock();
            media_sample_view_t sample_view;
            request.stream->process_sample(sample_view, request.rp, NULL);
            lock.lock();
        }
        else
        {
            samples_buffer.reset(new media_buffer_samples);
            // swap the source queue with an empty queue
            this->source->samples.swap(samples_buffer->samples);

            // TODO: this might be slow;
            // TODO: the pull rate should be twice of the samples
            // the encoder needs
            // TODO: the displaycapture should be implemented like this aswell;
            // currently there's a chance that a newer sample is assigned
            // to an older packet number

            // pass the sample to downstream
            lock.unlock();
            media_sample_t sample(new media_sample);
            sample->buffer = samples_buffer;
            media_sample_view_t sample_view(new media_sample_view(sample));
            request.stream->process_sample(sample_view, request.rp, NULL);
            lock.lock();
        }
    }
}

media_stream::result_t stream_loopback::request_sample(request_packet& rp, const media_stream*)
{
    source_loopback::request_t request;
    request.stream = this;
    request.rp = rp;

    this->source->requests.push(request);

    // dispatch the capture request
    const HRESULT hr = this->process_callback->mf_put_work_item(
        this->shared_from_this<stream_loopback>(),
        MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
    else if(hr == MF_E_SHUTDOWN)
        return FATAL_ERROR;

    return OK;
}

media_stream::result_t stream_loopback::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    return this->source->session->give_sample(this, sample_view, rp, true) ? OK : FATAL_ERROR;
}