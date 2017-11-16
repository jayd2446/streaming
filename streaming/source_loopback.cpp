#include "source_loopback.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <Mferror.h>
#include "assert.h"
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
    const float* in, bit_depth_t* out, bool silent)
{
    assert_(sizeof(bit_depth_t) <= sizeof(int32_t));
    // frame has a sample for each channel
    const UINT32 samples = frames * channels;
    for(UINT32 i = 0; i < samples; i++)
    {
        if(!silent)
        {
            // convert
            double sample;
            if(!this->generate_sine)
                sample = in[i];
            else
            {
                sample = sin(this->sine_var) * 0.05;
                if(i % channels == 0)
                    this->sine_var += 0.1;
            }
            int32_t sample_converted = (int32_t)(sample * std::numeric_limits<bit_depth_t>::max());

            // clamp
            if(sample_converted > std::numeric_limits<bit_depth_t>::max())
                sample_converted = std::numeric_limits<bit_depth_t>::max();
            else if(sample_converted < std::numeric_limits<bit_depth_t>::min())
                sample_converted = std::numeric_limits<bit_depth_t>::min();

            out[i] = (bit_depth_t)sample_converted;
        }
        else
            out[i] = 0;
    }
}

source_loopback::source_loopback(const media_session_t& session) : 
    media_source(session), device_time_position(0), started(false), 
    generate_sine(false), sine_var(0)
{
    HRESULT hr = S_OK;
    DWORD task_id;
    CHECK_HR(hr = MFLockSharedWorkQueue(L"Capture", 0, &task_id, &this->work_queue_id));

    this->process_callback.Attach(new async_callback_t(&source_loopback::process_cb, this->work_queue_id));
    this->serve_callback.Attach(new async_callback_t(&source_loopback::serve_cb));

    this->stream_base.time = this->stream_base.sample = -1;

done:
    if(FAILED(hr))
        throw std::exception();
}

source_loopback::~source_loopback()
{
    MFUnlockWorkQueue(this->work_queue_id);
}

HRESULT source_loopback::add_event_to_wait_queue()
{
    HRESULT hr = S_OK;

    //CComPtr<IMFAsyncResult> asyncresult;
    //CHECK_HR(hr = MFCreateAsyncResult(NULL, &this->process_callback->native, NULL, &asyncresult));
    //// use the priority of 1 for audio
    //CHECK_HR(hr = this->process_callback->mf_put_waiting_work_item(
    //    this->shared_from_this<source_loopback>(),
    //    this->process_event, 1, asyncresult, &this->callback_key));

    // schedule a work item for a half of the buffer duration
    CHECK_HR(hr = this->process_callback->mf_schedule_work_item(
        this->shared_from_this<source_loopback>(),
        -this->buffer_actual_duration / MILLISECOND_IN_TIMEUNIT / 2, &this->callback_key));

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
    /*ResetEvent(this->process_event);*/

    std::unique_lock<std::recursive_mutex> lock(this->process_mutex, std::try_to_lock);
    if(!lock.owns_lock())
        return;

    HRESULT hr = S_OK;
    // nextpacketsize and frames are equal
    UINT32 nextpacketsize = 0, frames = 0;
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

        bool silent = false;
        // set the stream time and sample base
        sample_base_t base = {-1, -1};
        if(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
        {
            // TODO: to combat against a possible drift in the timestamps
            // because of slightly inaccurate sample rate in a device,
            // set a new base periodically,
            // e.g once every second
            // (actually, this shouldn't be done because it can cause the
            // the audio buffer cutting operation to fail)
            std::cout << "DATA DISCONTINUITY" << std::endl;
            base.time = (LONGLONG)first_sample_timestamp;
            base.sample = (LONGLONG)devposition;
            if(this->generate_sine)
                base.time += SECOND_IN_TIME_UNIT / 200;
        }
        if(flags & AUDCLNT_BUFFERFLAGS_SILENT)
        {
            silent = true;
            std::cout << "SILENT" << std::endl;
        }
        else
            /*std::cout << "OK" << std::endl*/;

        // convert and copy to buffer
        BYTE* buffer_data;
        const UINT32 frame_len = this->get_block_align();
        CHECK_HR(hr = MFCreateMemoryBuffer(frames * frame_len, &buffer));
        CHECK_HR(hr = buffer->Lock(&buffer_data, NULL, NULL));
        convert_32bit_float_to_bitdepth_pcm(
            frames, this->channels, 
            (float*)data, (bit_depth_t*)buffer_data, silent);
        CHECK_HR(hr = buffer->Unlock());
        CHECK_HR(hr = buffer->SetCurrentLength(frames * frame_len));
        CHECK_HR(hr = sample->AddBuffer(buffer));

        // set the time and duration in frames
        CHECK_HR(hr = sample->SetSampleTime(devposition));
        CHECK_HR(hr = sample->SetSampleDuration(frames));

        /*static LONGLONG ts_ = std::numeric_limits<LONGLONG>::min();
        LONGLONG ts;

        ts = devposition;
        if(ts <= ts_)
            DebugBreak();
        ts_ = ts;*/

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

    // (the function isn't called constantly)
    //// TODO: devposition should be checked elsewhere if this function isn't called
    //// constantly
    //UINT64 devposition;
    //CHECK_HR(hr = this->audio_clock->GetPosition(&devposition, &this->device_time_position));

done:
    if(getbuffer)
        this->audio_capture_client->ReleaseBuffer(frames);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();

    lock.unlock();
    this->add_event_to_wait_queue();
    this->serve_requests();
}

void source_loopback::serve_cb(void*)
{
    // only one thread should be executing this
    std::unique_lock<std::recursive_mutex> lock2(this->serve_mutex, std::try_to_lock);
    if(!lock2.owns_lock())
        return;

    HRESULT hr = S_OK;
    request_t request;
    while(this->requests.get(request))
    {
        // clock is assumed to be valid if there's a request pending
        presentation_clock_t clock;
        if(!this->session->get_current_clock(clock))
            return;

        // samples are collected up to the request time;
        // sample that goes over the request time will not be collected

        // TODO: on data discontinuity the next base qpc might yeild an earlier timestamp
        // than the calculated timestamp on the last sample

        bool dispatch = false;
        media_buffer_samples_t samples(new media_buffer_samples);
        const double sample_duration = SECOND_IN_TIME_UNIT / (double)this->samples_per_second;
        const frame_unit request_end = (frame_unit)(request.rp.request_time / sample_duration);

        std::unique_lock<std::recursive_mutex> lock(this->samples_mutex);
        size_t consumed_samples = 0;
        for(auto it = this->samples.begin(); it != this->samples.end(); it++)
        {
            CComPtr<IMFSample> sample;
            CComPtr<IMFMediaBuffer> buffer, oldbuffer;
            DWORD buflen;
            sample_base_t stream_base;

            LONGLONG sample_pos, sample_dur;
            CHECK_HR(hr = (*it)->GetSampleTime(&sample_pos));
            CHECK_HR(hr = (*it)->GetSampleDuration(&sample_dur));
            CHECK_HR(hr = 
                (*it)->GetBlob(MF_MT_USER_DATA, (UINT8*)&stream_base, sizeof(sample_base_t), NULL));
            if(stream_base.time >= 0)
                this->stream_base = stream_base;
            assert_(this->stream_base.time >= 0);

            const frame_unit frame_base = (frame_unit)
                (clock->get_time_source()->system_time_to_time_source(this->stream_base.time) / 
                sample_duration);
            const frame_unit frame_pos = frame_base + (sample_pos - this->stream_base.sample);
            const frame_unit frame_end = frame_pos + sample_dur;

            // too new sample for the request
            if(frame_pos >= request_end)
            {
                dispatch = true;
                break;
            }
            // request can be dispatched
            if(frame_end >= request_end)
                dispatch = true;

            CHECK_HR(hr = (*it)->GetBufferByIndex(0, &oldbuffer));
            CHECK_HR(hr = oldbuffer->GetCurrentLength(&buflen));

            const frame_unit frame_diff_end = std::max(frame_end - request_end, 0LL);
            const DWORD offset_end  = (DWORD)frame_diff_end * this->get_block_align();

            assert_(((int)buflen - (int)offset_end) > 0);
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
                const LONGLONG new_sample_dur = offset_end / this->get_block_align();
                const LONGLONG new_sample_time = sample_pos + sample_dur - new_sample_dur;
                CHECK_HR(hr = (*it)->SetSampleTime(new_sample_time));
                CHECK_HR(hr = (*it)->SetSampleDuration(new_sample_dur));
            }
            else
                consumed_samples++;
            CHECK_HR(hr = MFCreateSample(&sample));
            CHECK_HR(hr = sample->AddBuffer(buffer));
            const frame_unit new_sample_time = frame_pos;
            const frame_unit new_sample_dur = sample_dur - frame_diff_end;
            CHECK_HR(hr = sample->SetSampleTime(new_sample_time));
            CHECK_HR(hr = sample->SetSampleDuration(new_sample_dur));

            if(!(request.rp.flags & AUDIO_DISCARD_PREVIOUS_SAMPLES))
                samples->samples.push_back(sample);
        }

        // TODO: stale requests should be checked other way
        // request is considered stale if the device time has already passed the request time
        // and there isn't any samples to serve
        if(request.rp.request_time <= (clock->get_current_time() - SECOND_IN_TIME_UNIT))
            dispatch = true;

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
            {
                sample_view.reset(new media_sample_view(samples));
                samples->bit_depth = sizeof(bit_depth_t);
                samples->channels = this->channels;
                samples->sample_rate = this->samples_per_second;
            }
            request.stream->process_sample(sample_view, request.rp, NULL);
        }
        else
            break;

        //// update the device position
        //{
        //    scoped_lock lock(this->process_mutex);
        //    // TODO: devposition should be checked elsewhere
        //    UINT64 devposition;
        //    CHECK_HR(hr = this->audio_clock->GetPosition(&devposition, &this->device_time_position));
        //}
        //const time_unit tp_device = 
        //    clock->get_time_source()->system_time_to_time_source((time_unit)this->device_time_position);

        //bool dispatch = false;
        //media_buffer_samples_t samples(new media_buffer_samples);
        //const time_unit request_time = request.rp.request_time;

        //// samples are served up to the request point;
        //// straddling sample is not consumed
        //std::unique_lock<std::recursive_mutex> lock(this->samples_mutex);
        //size_t consumed_samples = 0;
        //for(auto it = this->samples.begin(); it != this->samples.end(); it++)
        //{
        //    CComPtr<IMFSample> sample;
        //    CComPtr<IMFMediaBuffer> buffer, oldbuffer;
        //    DWORD buflen;
        //    sample_base_t stream_base;

        //    LONGLONG sample_time, sample_duration;
        //    CHECK_HR(hr = (*it)->GetSampleTime(&sample_time));
        //    CHECK_HR(hr = (*it)->GetSampleDuration(&sample_duration));
        //    CHECK_HR(hr = 
        //        (*it)->GetBlob(MF_MT_USER_DATA, (UINT8*)&stream_base, sizeof(sample_base_t), NULL));
        //    if(stream_base.time >= 0)
        //        this->stream_base = stream_base;
        //    assert_(this->stream_base.time >= 0);

        //    const double sample_dur = SECOND_IN_TIME_UNIT / (double)this->samples_per_second;
        //    // samples are fetched often which means that sample_duration * sample_dur
        //    // is unlikely to introduce rounding errors
        //    const time_unit tp_base = (time_unit)
        //        ((LONGLONG)(clock->get_time_source()->system_time_to_time_source(this->stream_base.time) /
        //        sample_dur) * sample_dur);
        //    const time_unit tp_start = tp_base +
        //        (time_unit)((sample_time - this->stream_base.sample) * sample_dur);
        //    const time_unit tp_end = tp_start + (time_unit)(sample_duration * sample_dur);

        //    // the tp base has shifted; dispatch the collected samples
        //    /*if(tp_base >= request_time)
        //        dispatch = true;*/
        //    // too new sample for the request
        //    if(tp_start >= request_time)
        //    {
        //        dispatch = true;
        //        break;
        //    }
        //    // request can be dispatched
        //    if(tp_end >= request_time)
        //        dispatch = true;

        //    CHECK_HR(hr = (*it)->GetBufferByIndex(0, &oldbuffer));
        //    CHECK_HR(hr = oldbuffer->GetCurrentLength(&buflen));

        //    const time_unit tp_diff_end = (time_unit)((LONGLONG)
        //        (std::max(tp_end - request_time, 0LL) / sample_dur) * sample_dur);
        //    // offset_end is in bytes
        //    const DWORD offset_end = (UINT32)(tp_diff_end / sample_dur) * this->get_block_align();


        //    assert_(((int)buflen - (int)offset_end) > 0);
        //    CHECK_HR(hr = MFCreateMediaBufferWrapper(
        //        oldbuffer, 0, buflen - offset_end, &buffer));
        //    CHECK_HR(hr = buffer->SetCurrentLength(buflen - offset_end));
        //    if(offset_end > 0)
        //    {
        //        // remove the consumed part of the old buffer
        //        CComPtr<IMFMediaBuffer> new_buffer;
        //        CHECK_HR(hr = MFCreateMediaBufferWrapper(
        //            oldbuffer, buflen - offset_end, offset_end, &new_buffer));
        //        CHECK_HR(hr = new_buffer->SetCurrentLength(offset_end));
        //        CHECK_HR(hr = (*it)->RemoveAllBuffers());
        //        CHECK_HR(hr = (*it)->AddBuffer(new_buffer));
        //        const LONGLONG sample_offset_end = offset_end / this->get_block_align();
        //        const LONGLONG new_sample_time = sample_time + sample_duration - sample_offset_end;
        //        const LONGLONG new_sample_dur = sample_offset_end;
        //        CHECK_HR(hr = (*it)->SetSampleTime(new_sample_time));
        //        CHECK_HR(hr = (*it)->SetSampleDuration(new_sample_dur));

        //        /*dispatch = true;*/
        //    }
        //    else
        //        consumed_samples++;
        //    CHECK_HR(hr = MFCreateSample(&sample));
        //    CHECK_HR(hr = sample->AddBuffer(buffer));
        //    const time_unit new_sample_time = tp_start;
        //    const time_unit new_sample_dur = (time_unit)(sample_duration * sample_dur) - tp_diff_end;
        //    CHECK_HR(hr = sample->SetSampleTime(new_sample_time));
        //    CHECK_HR(hr = sample->SetSampleDuration(new_sample_dur));

        //    if(!(request.rp.flags & AUDIO_DISCARD_PREVIOUS_SAMPLES))
        //        samples->samples.push_back(sample);

        //    /*if(dispatch)
        //        break;*/
        //}

        //// TODO: stale requests should be checked other way
        //// request is considered stale if the device time has already passed the request time
        //// and there isn't any samples to serve
        //if(/*consumed_samples == 0 && */request_time <= (clock->get_current_time() - SECOND_IN_TIME_UNIT))
        //    dispatch = true;
        ///*else if(consumed_samples > 0)
        //    dispatch = true;*/

        //if(dispatch)
        //{
        //    // erase all consumed samples
        //    for(size_t i = 0; i < consumed_samples; i++)
        //        this->samples.pop_front();

        //    // pop the request from the queue
        //    this->requests.pop(request);

        //    lock.unlock();
        //    // dispatch the request
        //    media_sample_view_t sample_view;
        //    if(!samples->samples.empty())
        //    {
        //        sample_view.reset(new media_sample_view(samples));
        //        samples->bit_depth = sizeof(bit_depth_t);
        //        samples->channels = this->channels;
        //        samples->sample_rate = this->samples_per_second;
        //    }
        //    request.stream->process_sample(sample_view, request.rp, NULL);
        //}
        //else
        //    break;
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

void source_loopback::serve_requests()
{
    const HRESULT hr = this->serve_callback->mf_put_work_item(this->shared_from_this<source_loopback>());
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
    else if(hr == MF_E_SHUTDOWN)
        return;
}

HRESULT source_loopback::initialize(bool capture)
{
    HRESULT hr = S_OK;

    CComPtr<IMMDeviceEnumerator> enumerator;
    CComPtr<IMMDevice> device;
    WAVEFORMATEX* engine_format = NULL;
    UINT32 buffer_frame_count;

    CHECK_HR(hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator));
    CHECK_HR(hr = enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &device));

    CHECK_HR(hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&this->audio_client));
    CHECK_HR(hr = this->audio_client->GetMixFormat(&engine_format));

    CHECK_HR(hr = this->audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, capture ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK, 
        0, BUFFER_DURATION, engine_format, NULL));

    CHECK_HR(hr = this->audio_client->GetBufferSize(&buffer_frame_count));

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

    // calculate the actual duration of the allocated buffer
    this->buffer_actual_duration = 
        (REFERENCE_TIME)((double)SECOND_IN_TIME_UNIT * buffer_frame_count / this->samples_per_second);

    //// create manual reset event handle
    //this->process_event.Attach(CreateEvent(
    //    NULL, TRUE, FALSE, NULL));  
    //if(!this->process_event)
    //    CHECK_HR(hr = E_FAIL);

    //CHECK_HR(hr = this->audio_client->SetEventHandle(this->process_event));

    //// set the event to mf queue
    //CHECK_HR(hr = this->add_event_to_wait_queue());

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
    source(source)
{
}

media_stream::result_t stream_loopback::request_sample(request_packet& rp, const media_stream*)
{
    source_loopback::request_t request;
    request.stream = this;
    request.rp = rp;

    this->source->requests.push(request);
    return OK;
}

media_stream::result_t stream_loopback::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    return this->source->session->give_sample(this, sample_view, rp, true) ? OK : FATAL_ERROR;
}