#include "source_loopback.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <Mferror.h>
#include <cassert>
#include <iostream>
#include <limits>
#include <cmath>

#undef max
#undef min

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
//static void CHECK_HR(HRESULT hr)
//{
//    if(FAILED(hr))
//        throw std::exception();
//}

void source_loopback::convert_32bit_float_to_bitdepth_pcm(
    UINT32 frames, UINT32 channels,
    const float* in, bit_depth_t* out)
{
    assert(sizeof(bit_depth_t) <= sizeof(int32_t));
    // frame has a sample for each channel
    const UINT32 samples = frames * channels;
    for(UINT32 i = 0; i < samples; i++)
    {
        // convert
        const double sample = in[i];
        int32_t sample_converted = (int32_t)(sample * std::numeric_limits<bit_depth_t>::max());

        // clamp
        if(sample_converted > std::numeric_limits<bit_depth_t>::max())
            sample_converted = std::numeric_limits<bit_depth_t>::max();
        else if(sample_converted < std::numeric_limits<bit_depth_t>::min())
            sample_converted = std::numeric_limits<bit_depth_t>::min();

        out[i] = (bit_depth_t)sample_converted;
    }
}

source_loopback::source_loopback(const media_session_t& session) : 
    media_source(session), device_time_position(0), started(false)
{
    this->process_callback.Attach(new async_callback_t(&source_loopback::process_cb));
    this->serve_callback.Attach(new async_callback_t(&source_loopback::serve_cb));

    this->stream_base.time = this->stream_base.sample = -1;
}

source_loopback::~source_loopback()
{
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

    std::unique_lock<std::recursive_mutex> lock(this->process_mutex, std::try_to_lock);
    if(!lock.owns_lock())
        return;

    HRESULT hr = S_OK;
    // nextpacketsize and frames are equal
    UINT32 nextpacketsize = 0, frames;
    bool getbuffer = false;
    while(SUCCEEDED(hr = this->audio_capture_client->GetNextPacketSize(&nextpacketsize)) && 
        nextpacketsize)
    {
        CComPtr<IMFSample> sample;
        CComPtr<IMFMediaBuffer> buffer;
        BYTE* data;
        DWORD flags;
        UINT64 first_sample_timestamp;
        UINT64 devposition;

        // TODO: source loopback must be reinitialized if the device becomes invalid

        // no excessive delay should happen between getbuffer and releasebuffer calls
        CHECK_HR(hr = this->audio_capture_client->GetBuffer(
            &data, &frames, &flags, &devposition, &first_sample_timestamp));
        getbuffer = true;
        if(frames == 0)
            break;

        CHECK_HR(hr = MFCreateSample(&sample));

        // set the stream time and sample base
        sample_base_t base = {-1, -1};
        if(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
        {
            std::cout << "DATA DISCONTINUITY" << std::endl;
            base.time = (LONGLONG)first_sample_timestamp;
            base.sample = (LONGLONG)devposition;
        }
        if(flags & AUDCLNT_BUFFERFLAGS_SILENT)
            std::cout << "SILENT" << std::endl;
        else
            /*std::cout << "OK" << std::endl*/;

        // convert and copy to buffer
        BYTE* buffer_data;
        const size_t frame_len = this->get_block_align();
        CHECK_HR(hr = MFCreateMemoryBuffer(frames * frame_len, &buffer));
        CHECK_HR(hr = buffer->Lock(&buffer_data, NULL, NULL));
        convert_32bit_float_to_bitdepth_pcm(
            frames, this->channels, (float*)data, (bit_depth_t*)buffer_data);
        CHECK_HR(hr = buffer->Unlock());
        CHECK_HR(hr = buffer->SetCurrentLength(frames * frame_len));
        CHECK_HR(hr = sample->AddBuffer(buffer));

        // set the time and duration in frames
        CHECK_HR(hr = sample->SetSampleTime(devposition));
        CHECK_HR(hr = sample->SetSampleDuration(frames));

        // add the base info
        CHECK_HR(hr = sample->SetBlob(MF_MT_USER_DATA, (UINT8*)&base, sizeof(sample_base_t)));

        // add sample
        {
            scoped_lock lock(this->samples_mutex);
            this->samples.push_back(sample);
        }

        getbuffer = false;
        CHECK_HR(hr = this->audio_capture_client->ReleaseBuffer(frames));
    }

    // TODO: devposition should be checked elsewhere
    UINT64 devposition;
    CHECK_HR(hr = this->audio_clock->GetPosition(&devposition, &this->device_time_position));

done:
    if(getbuffer)
        this->audio_capture_client->ReleaseBuffer(frames);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();

    lock.unlock();
    this->add_event_to_wait_queue();
}

void source_loopback::serve_cb(void*)
{
    // only one thread should be executing this
    std::unique_lock<std::recursive_mutex> lock(this->serve_mutex, std::try_to_lock);
    if(!lock.owns_lock())
        return;

    HRESULT hr = S_OK;
    request_t request;
    while(this->requests.get(request))
    {
        // clock is assumed to be valid if there's a request pending
        presentation_clock_t clock;
        if(!this->session->get_current_clock(clock))
            return;

        bool dispatch = false;
        media_buffer_samples_t samples(new media_buffer_samples);
        media_sample_t sample(new media_sample);
        sample->buffer = samples;

        const time_unit request_time = request.rp.request_time;

        // TODO: serve_cb should probably be called periodically, instead
        // of sink calling it

        // samples are served up to the request point;
        // straddling sample is not consumed
        std::unique_lock<std::recursive_mutex> lock(this->samples_mutex);
        size_t consumed_samples = 0;
        for(auto it = this->samples.begin(); it != this->samples.end(); it++)
        {
            CComPtr<IMFSample> sample;
            CComPtr<IMFMediaBuffer> buffer, oldbuffer;
            DWORD buflen;
            sample_base_t stream_base;

            LONGLONG sample_time, sample_duration;
            CHECK_HR(hr = (*it)->GetSampleTime(&sample_time));
            CHECK_HR(hr = (*it)->GetSampleDuration(&sample_duration));
            CHECK_HR(hr = 
                (*it)->GetBlob(MF_MT_USER_DATA, (UINT8*)&stream_base, sizeof(sample_base_t), NULL));
            if(stream_base.time >= 0)
                this->stream_base = stream_base;
            assert(this->stream_base.time >= 0);

            const double sample_dur = SECOND_IN_TIME_UNIT / (double)this->samples_per_second;
            // samples are fetched often which means that sample_duration * sample_dur
            // is unlikely to introduce rounding errors
            const time_unit tp_base = 
                clock->get_time_source()->system_time_to_time_source(this->stream_base.time);
            const time_unit tp_start = tp_base +
                (time_unit)((sample_time - this->stream_base.sample) * sample_dur);
            const time_unit tp_end = tp_start + (time_unit)(sample_duration * sample_dur);

            // the tp base has shifted; dispatch the collected samples
            if(tp_base >= request_time)
                dispatch = true;
            // too new sample for the request
            if(tp_start >= request_time)
                break;
            // request can be dispatched
            if(tp_end >= request_time)
                dispatch = true;

            CHECK_HR(hr = (*it)->GetBufferByIndex(0, &oldbuffer));
            CHECK_HR(hr = oldbuffer->GetCurrentLength(&buflen));

            const time_unit tp_diff_end = std::max(tp_end - request_time, 0LL);
            // offset_end is in bytes
            const DWORD offset_end = (UINT32)(tp_diff_end / sample_dur) * this->get_block_align();

            if(((int)buflen - (int)offset_end) <= 0)
                DebugBreak();
            CHECK_HR(hr = MFCreateMediaBufferWrapper(
                oldbuffer, 0, buflen - offset_end, &buffer));
            CHECK_HR(hr = buffer->SetCurrentLength(buflen - offset_end));
            if(offset_end > 0)
            {
                // remove the consumed part of the old buffer
                CComPtr<IMFMediaBuffer> new_buffer;
                CHECK_HR(hr = MFCreateMediaBufferWrapper(
                    oldbuffer, buflen - offset_end, offset_end, &new_buffer));
                CHECK_HR(hr = new_buffer->SetCurrentLength(offset_end));
                CHECK_HR(hr = (*it)->RemoveAllBuffers());
                CHECK_HR(hr = (*it)->AddBuffer(new_buffer));
                const LONGLONG sample_offset_end = offset_end / this->get_block_align();
                const LONGLONG new_sample_time = sample_time + sample_duration - sample_offset_end;
                const LONGLONG new_sample_dur = sample_offset_end;
                CHECK_HR(hr = (*it)->SetSampleTime(new_sample_time));
                CHECK_HR(hr = (*it)->SetSampleDuration(sample_offset_end));

                dispatch = true;
            }
            else
                consumed_samples++;
            CHECK_HR(hr = MFCreateSample(&sample));
            CHECK_HR(hr = sample->AddBuffer(buffer));
            const time_unit new_sample_time = tp_start;
            const time_unit new_sample_dur = (time_unit)(sample_duration * sample_dur) - tp_diff_end;
            CHECK_HR(hr = sample->SetSampleTime(new_sample_time));
            CHECK_HR(hr = sample->SetSampleDuration(new_sample_dur));

            if(!(request.rp.flags & AUDIO_DISCARD_PREVIOUS_SAMPLES))
                samples->samples.push_back(sample);

            if(dispatch)
                break;
        }

        // TODO: device time position might be updated before this thread is executed
        // request is considered stale if the device time has already passed the request time
        // and there isn't any samples to serve
        if(consumed_samples == 0 && request_time <=
            clock->get_time_source()->system_time_to_time_source(this->device_time_position))
            dispatch = true;
        /*else if(consumed_samples > 0)
            dispatch = true;*/

        if(dispatch)
        {
            // erase all consumed samples
            for(size_t i = 0; i < consumed_samples; i++)
                this->samples.pop_front();

            // pop the request from the queue
            this->requests.pop(request);

            lock.unlock();
            // dispatch the request
            media_sample_view_t sample_view;
            if(!samples->samples.empty())
                sample_view.reset(new media_sample_view(sample));
            request.stream->process_sample(sample_view, request.rp, NULL);
        }
        else
            break;
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

media_stream::result_t source_loopback::serve_requests()
{
    const HRESULT hr = this->serve_callback->mf_put_work_item(
        this->shared_from_this<source_loopback>(),
        MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
    else if(hr == MF_E_SHUTDOWN)
        return media_stream::FATAL_ERROR;

    return media_stream::OK;
}

HRESULT source_loopback::initialize()
{
    HRESULT hr = S_OK;

    CComPtr<IMMDeviceEnumerator> enumerator;
    CComPtr<IMMDevice> device;
    WAVEFORMATEX* engine_format = NULL;

    CHECK_HR(hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator));
    CHECK_HR(hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));

    CHECK_HR(hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&this->audio_client));
    CHECK_HR(hr = this->audio_client->GetMixFormat(&engine_format));

    CHECK_HR(hr = this->audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 
        0, 0, engine_format, NULL));

    CHECK_HR(hr = this->audio_client->GetService(
        __uuidof(IAudioCaptureClient), (void**)&this->audio_capture_client));

    CHECK_HR(hr = this->audio_client->GetService(
        __uuidof(IAudioClock), (void**)&this->audio_clock));

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

    // create manual reset event handle
    this->process_event.Attach(CreateEvent(
        NULL, TRUE, FALSE, NULL));  
    if(!this->process_event)
        CHECK_HR(hr = E_FAIL);

    CHECK_HR(hr = this->audio_client->SetEventHandle(this->process_event));

    // set the event to mf queue
    CHECK_HR(hr = this->add_event_to_wait_queue());

    // start capturing
    CHECK_HR(hr = this->start());

done:
    if(engine_format)
        CoTaskMemFree(engine_format);
    if(FAILED(hr))
        throw std::exception();

    return hr;
}

HRESULT source_loopback::start()
{
    HRESULT hr = S_OK;
    if(!this->started)
    {
        CHECK_HR(hr = this->audio_client->Start());
        this->started = true;
    }

done:
    return hr;
}

media_stream_t source_loopback::create_stream()
{
    return media_stream_t(new stream_loopback(this->shared_from_this<source_loopback>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_loopback::stream_loopback(const source_loopback_t& source) : 
    source(source),
    sample(new media_sample)
{
}

media_stream::result_t stream_loopback::request_sample(request_packet& rp, const media_stream*)
{
    source_loopback::request_t request;
    request.stream = this;
    request.rp = rp;

    this->source->requests.push(request);
    return this->source->serve_requests();
}

media_stream::result_t stream_loopback::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    return this->source->session->give_sample(this, sample_view, rp, true) ? OK : FATAL_ERROR;
}