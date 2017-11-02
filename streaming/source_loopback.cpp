#include "source_loopback.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <Mferror.h>
#include <cassert>
#include <iostream>
#include <cmath>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
//static void CHECK_HR(HRESULT hr)
//{
//    if(FAILED(hr))
//        throw std::exception();
//}
#undef max
#undef min

#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000

template<typename T>
void copy_aligned(BYTE* to, const BYTE* from)
{
    *((T*)to) = *((const T*)from);
}

source_loopback::thread_capture::thread_capture(source_loopback_t& source) :
    source(source),
    running(false)
{
    this->callback.Attach(new async_callback_t(&thread_capture::serve_cb));
}

bool source_loopback::thread_capture::on_clock_start(time_unit t)
{
    if(this->running)
        return true;

    this->running = true;
    this->scheduled_callback(t);
    return true;
}

void source_loopback::thread_capture::on_clock_stop(time_unit t)
{
    if(!this->running)
        return;

    this->running = false;
}

bool source_loopback::thread_capture::get_clock(presentation_clock_t& clock)
{
    source_loopback_t source;
    if(this->get_source(source))
    {
        clock = source->get_device_clock();
        return !!clock;
    }
    else
        return false;
}

void source_loopback::thread_capture::scheduled_callback(time_unit due_time)
{
    // TODO: read the buffer, serve request packets and schedule a new callback
    source_loopback_t source;
    if(!this->get_source(source))
    {
        this->on_clock_stop(due_time);
        return;
    }
    if(!this->running)
        return;

    this->capture_buffer(source);
    this->serve_requests();
    this->schedule_new(due_time);
}

void source_loopback::thread_capture::capture_buffer(const source_loopback_t& source)
{
    HRESULT hr = S_OK;

    // nextpacketsize and frames are equal
    UINT32 nextpacketsize = 0, frames;
    bool getbuffer = false;
    while(SUCCEEDED(hr = source->audio_capture_client->GetNextPacketSize(&nextpacketsize)) && 
        nextpacketsize)
    {
        CComPtr<IMFSample> sample;
        CComPtr<IMFMediaBuffer> buffer;
        BYTE* data;
        DWORD flags;
        UINT64 first_sample_timestamp;

        CHECK_HR(hr = MFCreateSample(&sample));

        // no excessive delay should happen between getbuffer and releasebuffer calls
        CHECK_HR(hr = source->audio_capture_client->GetBuffer(
            &data, &frames, &flags, NULL, &first_sample_timestamp));
        getbuffer = true;

        if(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
            std::cout << "DATA DISCONTINUITY" << std::endl;
        if(flags & AUDCLNT_BUFFERFLAGS_SILENT)
            std::cout << "SILENT" << std::endl;
        else
            /*std::cout << "OK" << std::endl*/;

        BYTE* buffer_data;
        CHECK_HR(hr = MFCreateMemoryBuffer(frames * 4, &buffer));
        CHECK_HR(hr = buffer->Lock(&buffer_data, NULL, NULL));
        // and varies in the interval [-1.0, 1.0], thus signed values.
        static double incr = 0;
        for(UINT32 i = 0; i < (frames * source->channels); i++)
        {
            double sample = ((float*)data)[i];
            int32_t sample_converted = (int32_t)(sample * std::numeric_limits<int16_t>::max());
            if(sample_converted > std::numeric_limits<int16_t>::max())
                sample_converted = std::numeric_limits<int16_t>::max();
            else if(sample_converted < std::numeric_limits<int16_t>::min())
                sample_converted = std::numeric_limits<int16_t>::min();

            // sin(incr) * std::numeric_limits<int16_t>::max() / 2.0;
            ((int16_t*)buffer_data)[i] = (int16_t)sample_converted;
            if((i % source->channels) == 0)
                incr += 0.1;
        }
        CHECK_HR(hr = buffer->Unlock());
        CHECK_HR(hr = buffer->SetCurrentLength(frames * 4));

        CHECK_HR(hr = sample->AddBuffer(buffer));

        static LONGLONG sample_dur = 0;
        CHECK_HR(hr = sample->SetSampleDuration(
            SECOND_IN_TIME_UNIT *
            (UINT64)frames / (UINT64)source->samples_per_second));
        CHECK_HR(hr = sample->SetSampleTime(/*sample_dur*/first_sample_timestamp));

        sample_dur += SECOND_IN_TIME_UNIT * (UINT64)frames / (UINT64)source->samples_per_second;

        // add sample
        {
            scoped_lock lock(this->samples_mutex);
            this->samples.push_back(sample);
        }

        getbuffer = false;
        CHECK_HR(hr = source->audio_capture_client->ReleaseBuffer(frames));
    }

    UINT64 devposition;
    CHECK_HR(hr = source->audio_clock->GetPosition(&devposition, &this->device_time_position));

done:
    if(getbuffer)
        source->audio_capture_client->ReleaseBuffer(frames);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
}

void source_loopback::thread_capture::serve_requests()
{
    HRESULT hr = S_OK;

    CHECK_HR(hr = this->callback->mf_put_work_item(
        this->shared_from_this<thread_capture>(), MFASYNC_CALLBACK_QUEUE_MULTITHREADED));

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();
}

void source_loopback::thread_capture::serve_cb(void*)
{
    // TODO: only one thread should be executing this
    /*scoped_lock serve_lock(this->serve_mutex);*/

    time_unit last_due_time = 0;

    source_loopback_t source;
    if(!this->get_source(source))
        return;

    HRESULT hr = S_OK;
    request_t request;
    while(source->requests.get(request))
    {
        // clock is assumed to be valid if there's a request pending
        presentation_clock_t clock;
        if(!source->session->get_current_clock(clock))
            return;

        bool dispatch = false;
        media_buffer_samples_t samples(new media_buffer_samples);
        media_sample_t sample(new media_sample);
        sample->buffer = samples;

        const time_unit request_time = request.rp.request_time;

        // samples are served up to the request point
        std::unique_lock<std::recursive_mutex> lock(this->samples_mutex);
        size_t consumed_samples = 0;
        for(auto it = this->samples.begin(); it != this->samples.end(); it++)
        {
            CComPtr<IMFSample> sample;
            CComPtr<IMFMediaBuffer> buffer, oldbuffer;
            DWORD buflen;

            LONGLONG sample_time, sample_duration;
            time_unit tp_start, tp_end;
            CHECK_HR(hr = (*it)->GetSampleTime(&sample_time));
            CHECK_HR(hr = (*it)->GetSampleDuration(&sample_duration));
            // sample_time;//
            tp_start = clock->get_time_source()->system_time_to_time_source(sample_time);
            tp_end = tp_start + sample_duration;

            // too new sample for the request
            if(tp_start >= request_time)
                break;
            // request can be dispatched
            if(tp_end >= request_time)
                dispatch = true;

            CHECK_HR(hr = (*it)->GetBufferByIndex(0, &oldbuffer));
            CHECK_HR(hr = oldbuffer->GetCurrentLength(&buflen));

            // TODO: floating point operation might introduce errors
            const time_unit tp_diff_end = std::max(tp_end - request_time, 0LL);
            const DWORD offset_end =
                (UINT32)(tp_diff_end / (SECOND_IN_TIME_UNIT / (double)source->samples_per_second))
                * 4; // block align is 4 because the buffer is converted to 2 channel 16 bit pcm

            if(((int)buflen - (int)offset_end) <= 0)
                DebugBreak();
            CHECK_HR(hr = MFCreateMediaBufferWrapper(
                oldbuffer, 0, buflen - offset_end, &buffer));
            CHECK_HR(hr = buffer->SetCurrentLength(buflen - offset_end));
            if(offset_end > 0)
            {
                CComPtr<IMFMediaBuffer> new_buffer;
                CHECK_HR(hr = MFCreateMediaBufferWrapper(
                    oldbuffer, buflen - offset_end, offset_end, &new_buffer));
                CHECK_HR(hr = new_buffer->SetCurrentLength(offset_end));
                CHECK_HR(hr = (*it)->RemoveAllBuffers());
                CHECK_HR(hr = (*it)->AddBuffer(new_buffer));
                const LONGLONG new_sample_time = sample_time + sample_duration - tp_diff_end;
                const LONGLONG new_sample_dur = tp_diff_end;
                CHECK_HR(hr = (*it)->SetSampleTime(new_sample_time));
                CHECK_HR(hr = (*it)->SetSampleDuration(new_sample_dur));
            }
            else
                consumed_samples++;
            CHECK_HR(hr = MFCreateSample(&sample));
            CHECK_HR(hr = sample->AddBuffer(buffer));
            const time_unit new_sample_time = tp_start;
            const time_unit new_sample_dur = sample_duration - tp_diff_end;
            CHECK_HR(hr = sample->SetSampleTime(new_sample_time));
            CHECK_HR(hr = sample->SetSampleDuration(new_sample_dur));

            /*std::cout << new_sample_time << std::endl;*/
            if(!(request.rp.flags & AUDIO_DISCARD_PREVIOUS_SAMPLES))
                samples->samples.push_back(sample);
        }

        // TODO: device time position might be updated before this thread is executed
        // request is considered stale if the device time has already passed the request time
        // and there isn't any samples to serve
        if(consumed_samples == 0 && request_time <=
            clock->get_time_source()->system_time_to_time_source(this->device_time_position))
            dispatch = true;
        else if(consumed_samples > 0)
            dispatch = true;

        if(dispatch)
        {
            // erase all consumed samples
            for(size_t i = 0; i < consumed_samples; i++)
                this->samples.pop_front();

            // pop the request from the queue
            source->requests.pop(request);

            lock.unlock();
            // dispatch the request
            media_sample_view_t sample_view;
            if(!samples->samples.empty())
                sample_view.reset(new media_sample_view(sample));
            request.stream->process_sample(sample_view, request.rp, NULL);
        }
        else
            break;



    //loop:
    //    for(auto it = this->samples.begin(); it != this->samples.end(); it++)
    //    {
    //        CComPtr<IMFSample> sample;
    //        CComPtr<IMFMediaBuffer> buffer, oldbuffer;
    //        DWORD buflen;

    //        LONGLONG sample_time, sample_duration;
    //        time_unit tp_start, tp_end;
    //        CHECK_HR(hr = (*it)->GetSampleTime(&sample_time));
    //        CHECK_HR(hr = (*it)->GetSampleDuration(&sample_duration));
    //        tp_start = clock->get_time_source()->system_time_to_time_source(sample_time);
    //        tp_end = tp_start + sample_duration;

    //        // stale sample
    //        if(tp_end <= request_start)
    //        {
    //            assert(processed_samples == 0);
    //            this->samples.pop_front();
    //            goto loop;
    //        }
    //        // too new sample for the request
    //        if(tp_start >= request_end)
    //            break;
    //        // request can be dispatched
    //        if(tp_end >= request_end)
    //            dispatch = true;

    //        CHECK_HR(hr = (*it)->GetBufferByIndex(0, &oldbuffer));
    //        CHECK_HR(hr = oldbuffer->GetCurrentLength(&buflen));

    //        // TODO: floating point operation might introduce errors
    //        // wrap a buffer around the sample that has request_time as the sample time
    //        const time_unit tp_diff_start = std::max(request_start - tp_start, 0LL);
    //        const DWORD offset_start = 
    //            (UINT32)(tp_diff_start / (SECOND_IN_TIME_UNIT / (double)source->samples_per_second))
    //            * source->block_align;
    //        const time_unit tp_diff_end = std::max(tp_end - (request_end + tp_start), 0LL);
    //        const DWORD offset_end = 
    //            (UINT32)(tp_diff_end / (SECOND_IN_TIME_UNIT / (double)source->samples_per_second))
    //            * source->block_align;

    //        CHECK_HR(hr = MFCreateMediaBufferWrapper(
    //            oldbuffer, offset_start, buflen - offset_end, &buffer));
    //        // set a new buffer for the sample if it's partially consumed
    //        if(offset_end > 0)
    //        {
    //            CComPtr<IMFMediaBuffer> new_buffer;
    //            CHECK_HR(hr = MFCreateMediaBufferWrapper(
    //                oldbuffer, buflen - offset_end, offset_end, &new_buffer));
    //            CHECK_HR(hr = (*it)->RemoveAllBuffers());
    //            CHECK_HR(hr = (*it)->AddBuffer(new_buffer));
    //            // tp_diff_end is in same units as the native sample times
    //            const LONGLONG new_sample_time = sample_time + sample_duration - tp_diff_end;
    //            const LONGLONG new_sample_dur = sample_time + sample_duration - new_sample_time;
    //            CHECK_HR(hr = (*it)->SetSampleTime(new_sample_time));
    //            CHECK_HR(hr = (*it)->SetSampleDuration(new_sample_dur));
    //        }
    //        else
    //            processed_samples++;
    //        CHECK_HR(hr = MFCreateSample(&sample));
    //        CHECK_HR(hr = sample->AddBuffer(buffer));
    //        const time_unit new_sample_time = tp_start + tp_diff_start;
    //        const time_unit new_sample_dur = sample_duration - tp_diff_start - tp_diff_end;
    //        assert(new_sample_dur > 0);
    //        CHECK_HR(hr = sample->SetSampleTime(new_sample_time));
    //        CHECK_HR(hr = sample->SetSampleDuration(new_sample_dur));

    //        samples->samples.push_back(sample);
    //    }

    //    // TODO: device time position might be updated before this thread is executed
    //    // request is considered stale if the device time has already passed the request's
    //    // end time and there isn't any samples generated
    //    if(this->samples.empty() && request_end <= 
    //        clock->get_time_source()->system_time_to_time_source(this->device_time_position))
    //        dispatch = true;

    //    if(dispatch)
    //    {
    //        // erase all processed samples
    //        for(size_t i = 0; i < processed_samples; i++)
    //            this->samples.pop_front();

    //        // pop the request from the queue
    //        source->requests.pop(request);

    //        lock.unlock();
    //        // dispatch the request
    //        media_sample_view_t sample_view;
    //        if(!samples->samples.empty())
    //            sample_view.reset(new media_sample_view(sample));
    //        request.stream->process_sample(sample_view, request.rp, NULL);
    //    }
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

bool source_loopback::thread_capture::get_source(source_loopback_t& source)
{
    source = this->source.lock();
    return !!source;
}

void source_loopback::thread_capture::schedule_new(time_unit due_time)
{
    presentation_clock_t t;
    if(this->get_clock(t))
    {
        const time_unit current_time = t->get_current_time();
        time_unit scheduled_time = this->get_next_due_time(due_time);

        if(!this->schedule_new_callback(scheduled_time))
        {
            if(scheduled_time > current_time)
            {
                std::cout << "VERY CLOSE in source_loopback" << std::endl;
                this->scheduled_callback(scheduled_time);
            }
            else
            {
                do
                {
                    const time_unit current_time = t->get_current_time();
                    scheduled_time = current_time;

                    std::cout << "AUDIO CAPTURE THREAD MISSED AT LEAST ONCE" << std::endl;
                    scheduled_time = this->get_next_due_time(scheduled_time);
                }
                while(!this->schedule_new_callback(scheduled_time));
            }
        }
    }
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


source_loopback::source_loopback(const media_session_t& session) : 
    media_source(session), mf_alignment(-1), device_time_position(0)
{
    this->started = false;
    this->process_callback.Attach(new async_callback_t(&source_loopback::process_cb));
    this->serve_callback.Attach(new async_callback_t(&source_loopback::serve_cb));
}

source_loopback::~source_loopback()
{
    if(this->capture_thread)
        this->capture_thread->running = false;
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

        CHECK_HR(hr = MFCreateSample(&sample));

        // no excessive delay should happen between getbuffer and releasebuffer calls
        CHECK_HR(hr = this->audio_capture_client->GetBuffer(
            &data, &frames, &flags, &devposition, &first_sample_timestamp));
        getbuffer = true;
        if(frames == 0)
            break;

        sample_base_t base = {-1, -1};
        if(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
        {
            std::cout << "DATA DISCONTINUITY" << std::endl;
            base.samples_timebase = first_sample_timestamp;
            base.samples_samplebase = devposition;
        }
        if(flags & AUDCLNT_BUFFERFLAGS_SILENT)
            std::cout << "SILENT" << std::endl;
        else
            /*std::cout << "OK" << std::endl*/;

        BYTE* buffer_data;
        CHECK_HR(hr = MFCreateMemoryBuffer(frames * 4, &buffer));
        CHECK_HR(hr = buffer->Lock(&buffer_data, NULL, NULL));
        // and varies in the interval [-1.0, 1.0], thus signed values.
        static double incr = 0;
        for(UINT32 i = 0; i < (frames * this->channels); i++)
        {
            double sample = ((float*)data)[i];
            int32_t sample_converted = (int32_t)(sample * std::numeric_limits<int16_t>::max());
            if(sample_converted > std::numeric_limits<int16_t>::max())
                sample_converted = std::numeric_limits<int16_t>::max();
            else if(sample_converted < std::numeric_limits<int16_t>::min())
                sample_converted = std::numeric_limits<int16_t>::min();

            // sin(incr) * std::numeric_limits<int16_t>::max() / 2.0;
            ((int16_t*)buffer_data)[i] = (int16_t)sample_converted;
            if((i % this->channels) == 0)
                incr += 0.1;
        }
        CHECK_HR(hr = buffer->Unlock());
        CHECK_HR(hr = buffer->SetCurrentLength(frames * 4));

        CHECK_HR(hr = sample->AddBuffer(buffer));

        static LONGLONG sample_dur = 0;
        static UINT64 ts = 0;
        /*CHECK_HR(hr = sample->SetSampleDuration(
            SECOND_IN_TIME_UNIT *
            (UINT64)frames / (UINT64)this->samples_per_second));
        CHECK_HR(hr = sample->SetSampleTime(first_sample_timestamp));*/
        CHECK_HR(hr = sample->SetSampleTime(devposition));
        CHECK_HR(hr = sample->SetSampleDuration(frames));

        CHECK_HR(hr = sample->SetBlob(MF_MT_USER_DATA, (UINT8*)&base, sizeof(sample_base_t)));

        /*std::cout << "ts: " << first_sample_timestamp << ", ts+dur: " <<
            first_sample_timestamp + (SECOND_IN_TIME_UNIT *
            (UINT64)frames / (UINT64)this->samples_per_second) << std::endl;*/

        if(first_sample_timestamp <= ts)
            DebugBreak();
        ts = first_sample_timestamp;

        /*std::cout << devposition << ", " << devposition + frames << std::endl;*/

        sample_dur += SECOND_IN_TIME_UNIT * (UINT64)frames / (UINT64)this->samples_per_second;

        // add sample
        {
            scoped_lock lock(this->samples_mutex);
            this->samples.push_back(sample);
        }

        getbuffer = false;
        CHECK_HR(hr = this->audio_capture_client->ReleaseBuffer(frames));
    }

    UINT64 devposition;
    CHECK_HR(hr = this->audio_clock->GetPosition(&devposition, &this->device_time_position));

done:
    if(getbuffer)
        this->audio_capture_client->ReleaseBuffer(frames);
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw std::exception();

    this->add_event_to_wait_queue();
}

void source_loopback::serve_cb(void*)
{
    // TODO: only one thread should be executing this
    scoped_lock serve_lock(this->serve_mutex);

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

        // samples are served up to the request point
        std::unique_lock<std::recursive_mutex> lock(this->samples_mutex);
        size_t consumed_samples = 0;
        for(auto it = this->samples.begin(); it != this->samples.end(); it++)
        {
            CComPtr<IMFSample> sample;
            CComPtr<IMFMediaBuffer> buffer, oldbuffer;
            DWORD buflen;
            sample_base_t base;

            LONGLONG sample_time, sample_duration;
            time_unit tp_start, tp_end;
            CHECK_HR(hr = (*it)->GetSampleTime(&sample_time));
            CHECK_HR(hr = (*it)->GetSampleDuration(&sample_duration));
            CHECK_HR(hr = (*it)->GetBlob(MF_MT_USER_DATA, (UINT8*)&base, sizeof(sample_base_t), NULL));
            if(base.samples_timebase >= 0)
                this->samples_base = base;
            // sample_time;//
            // clock->get_time_source()->system_time_to_time_source(sample_time);
            tp_start = (time_unit)((sample_time - this->samples_base.samples_samplebase) * (SECOND_IN_TIME_UNIT / (double)this->samples_per_second));
            tp_start += clock->get_time_source()->system_time_to_time_source(this->samples_base.samples_timebase);
            tp_end = tp_start + (time_unit)(sample_duration * (SECOND_IN_TIME_UNIT / (double)this->samples_per_second));

            // too new sample for the request
            if(tp_start >= request_time)
                break;
            // request can be dispatched
            if(tp_end >= request_time)
                dispatch = true;

            /*static time_unit tpp = tp_start - 1;
            if(tp_start <= tpp)
                DebugBreak();
            tpp = tp_start;*/
            /*std::cout << tp_start << std::endl;*/

            CHECK_HR(hr = (*it)->GetBufferByIndex(0, &oldbuffer));
            CHECK_HR(hr = oldbuffer->GetCurrentLength(&buflen));

            // TODO: floating point operation might introduce errors
            time_unit tp_diff_end = std::max(tp_end - request_time, 0LL);
            const DWORD offset_end =
                (UINT32)(tp_diff_end / (SECOND_IN_TIME_UNIT / (double)this->samples_per_second))
                * 4; // block align is 4 because the buffer is converted to 2 channel 16 bit pcm
            tp_diff_end = offset_end / 4;

            if(((int)buflen - (int)offset_end) <= 0)
                DebugBreak();
            CHECK_HR(hr = MFCreateMediaBufferWrapper(
                oldbuffer, 0, buflen - offset_end, &buffer));
            CHECK_HR(hr = buffer->SetCurrentLength(buflen - offset_end));
            if(offset_end > 0)
            {
                CComPtr<IMFMediaBuffer> new_buffer;
                CHECK_HR(hr = MFCreateMediaBufferWrapper(
                    oldbuffer, buflen - offset_end, offset_end, &new_buffer));
                CHECK_HR(hr = new_buffer->SetCurrentLength(offset_end));
                CHECK_HR(hr = (*it)->RemoveAllBuffers());
                CHECK_HR(hr = (*it)->AddBuffer(new_buffer));
                const LONGLONG new_sample_time = sample_time + sample_duration - tp_diff_end;
                const LONGLONG new_sample_dur = tp_diff_end;
                CHECK_HR(hr = (*it)->SetSampleTime(sample_time + buflen / 4 - offset_end / 4));
                CHECK_HR(hr = (*it)->SetSampleDuration(offset_end / 4));

                // the new sample time might have a greater start time than the next sample time
                // in queue
                // because the durations and start points overlap
            }
            else
                consumed_samples++;
            CHECK_HR(hr = MFCreateSample(&sample));
            CHECK_HR(hr = sample->AddBuffer(buffer));
            const time_unit new_sample_time = tp_start;
            const time_unit new_sample_dur = (time_unit)(sample_duration * (SECOND_IN_TIME_UNIT / (double)this->samples_per_second)) - tp_diff_end;
            CHECK_HR(hr = sample->SetSampleTime(new_sample_time));
            CHECK_HR(hr = sample->SetSampleDuration(new_sample_dur));

            /*std::cout << new_sample_time << std::endl;*/
            if(!(request.rp.flags & AUDIO_DISCARD_PREVIOUS_SAMPLES))
                samples->samples.push_back(sample);
        }

        // TODO: device time position might be updated before this thread is executed
        // request is considered stale if the device time has already passed the request time
        // and there isn't any samples to serve
        if(consumed_samples == 0 && request_time <=
            clock->get_time_source()->system_time_to_time_source(this->device_time_position))
            dispatch = true;
        else if(consumed_samples > 0)
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

presentation_clock_t source_loopback::get_device_clock()
{
    return presentation_clock_t(std::atomic_load(&this->capture_thread_clock));
}

void source_loopback::pull_buffer(media_sample_view_t& sample_view, const request_packet& rp)
{
    /*scoped_lock lock(this->capture_mutex);*/


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

    //// initialize the capture thread
    //assert(!this->capture_thread);
    //assert(!this->capture_thread_clock);
    //this->capture_thread_time.reset(new presentation_time_source);
    //this->capture_thread_clock.reset(new presentation_clock(this->capture_thread_time));
    //this->capture_thread.reset(new thread_capture(this->shared_from_this<source_loopback>()));
    //this->capture_thread->register_sink(this->capture_thread_clock);
    //// every 500ms
    //this->capture_thread->set_pull_rate(4, 1);

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
    {
        CHECK_HR(hr = this->audio_client->Start());
        /*this->capture_thread_clock->clock_start(0);*/
    }

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
    //// start capturing
    //this->source->start();

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

    return this->source->serve_requests();

    //// dispatch the capture request
    //const HRESULT hr = this->process_callback->mf_put_work_item(
    //    this->shared_from_this<stream_loopback>(),
    //    MFASYNC_CALLBACK_QUEUE_MULTITHREADED);
    //if(FAILED(hr) && hr != MF_E_SHUTDOWN)
    //    throw std::exception();
    //else if(hr == MF_E_SHUTDOWN)
    //    return FATAL_ERROR;

    /*return OK;*/
}

media_stream::result_t stream_loopback::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    return this->source->session->give_sample(this, sample_view, rp, true) ? OK : FATAL_ERROR;
}