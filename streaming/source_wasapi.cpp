#include "source_wasapi.h"
#include "assert.h"
#include "control_pipeline2.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <Mferror.h>
#include <iostream>
#include <limits>
#include <cmath>

#undef max
#undef min

#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#define AUTOCONVERT_PCM (0)
#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
//static void CHECK_HR(HRESULT hr)
//{
//    if(FAILED(hr))
//        throw HR_EXCEPTION(hr);
//}

extern DWORD capture_work_queue_id;
extern LONG capture_audio_priority;

source_wasapi::source_wasapi(const media_session_t& session) : 
    media_source(session), started(false), 
    consumed_samples_end(std::numeric_limits<frame_unit>::min()),
    next_frame_position(std::numeric_limits<frame_unit>::min()),
    capture(false), broken(false), wait_queue(false),
    frame_base(std::numeric_limits<frame_unit>::min()),
    devposition_base(std::numeric_limits<frame_unit>::min()),
    work_queue_id(::capture_work_queue_id)
{
    // the process callback really cannot be in higher priority mode,
    // because it can cause problems with the work queue under a high load
    this->process_callback.Attach(new async_callback_t(&source_wasapi::process_cb/*, this->work_queue_id*/));
}

source_wasapi::~source_wasapi()
{
    // the serve_cb will automatically serve all the requests before this
    // destructor is called, because the process_cb periodically calls the serve_cb
    // and the serve_cb indirectionally will release the last reference to this component;
    // the stream won't make new requests either because the topology has already been changed;
    // this means that this destructor doesn't need to flush the request queue

    // even though the request queue will be empty,
    // there might be a standing waiting work item that won't be freed before shutting down
    // the work queue

    HRESULT hr = S_OK;

    if(this->started)
    {
        hr = this->audio_client->Stop();
        if(!this->capture)
            hr = this->audio_client_render->Stop();
    }
    if(this->wait_queue)
        // no source_wasapi functions can be running when @ the destructor;
        // cancel the outstanding waiting item so that it won't
        // remain in the work queue(there exists a theoretical chance that
        // the process_work_key is assigned to another item, but it is a 64 bit random int,
        // the chance of broken and lastly the small time window between last fired scheduled item
        // and this call causes the chance of collision to be rather nonexistent)
        hr = MFCancelWorkItem(this->process_work_key);
}

HRESULT source_wasapi::add_event_to_wait_queue()
{
    HRESULT hr = S_OK;

    CComPtr<IMFAsyncResult> asyncresult;
    CHECK_HR(hr = this->process_callback->mf_schedule_work_item(
        this->shared_from_this<source_wasapi>(), 
        capture_interval_ms, &this->process_work_key));

done:
    return hr;
}

HRESULT source_wasapi::create_waveformat_type(WAVEFORMATEX* format)
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

void source_wasapi::serve_cb(void*)
{
    // reset the scene to recreate this component
    if(this->broken)
        this->request_reinitialization(this->ctrl_pipeline);

    // try to serve
    HRESULT hr = S_OK;
    request_t request;
    while(this->requests.get(request))
    {
        // samples are collected up to the request time;
        // sample that goes over the request time will not be collected;
        // cutting operation is preferred so that when changing scene the audio plays
        // up to the switch point

        bool dispatch = false;

        // use the stream's sample
        stream_wasapi* stream = reinterpret_cast<stream_wasapi*>(request.stream);
        media_sample_audio& audio = stream->audio_sample;
        audio.buffer->samples.clear();
        audio.bit_depth = sizeof(bit_depth_t) * 8;
        audio.channels = this->channels;
        audio.sample_rate = this->samples_per_second;

        const double sample_duration = SECOND_IN_TIME_UNIT / (double)this->samples_per_second;
        frame_unit request_end = (frame_unit)(request.rp.request_time / sample_duration);

        size_t consumed_samples = 0;
        frame_unit consumed_samples_end = this->consumed_samples_end;
        for(auto it = this->buffer.samples.begin(); it != this->buffer.samples.end() && !dispatch; 
            it++)
        {
            CComPtr<IMFSample> sample;
            CComPtr<IMFMediaBuffer> buffer, oldbuffer;
            DWORD buflen;

            LONGLONG sample_pos, sample_dur;
            CHECK_HR(hr = (*it)->GetSampleTime(&sample_pos));
            CHECK_HR(hr = (*it)->GetSampleDuration(&sample_dur));

            const frame_unit frame_pos = sample_pos;
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
            const DWORD offset_end  = (DWORD)frame_diff_end * this->block_align;
            const frame_unit new_sample_time = sample_pos;
            const frame_unit new_sample_dur = sample_dur - frame_diff_end;

            if(new_sample_time < consumed_samples_end)
                std::cout << "wrong timestamp in source_wasapi" << std::endl;

            assert_(((int)buflen - (int)offset_end) > 0);
            CHECK_HR(hr = MFCreateMediaBufferWrapper(oldbuffer, 0, buflen - offset_end, &buffer));
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
                const LONGLONG new_sample_dur = offset_end / this->block_align;
                const LONGLONG new_sample_time = sample_pos + sample_dur - new_sample_dur;
                CHECK_HR(hr = (*it)->SetSampleTime(new_sample_time));
                CHECK_HR(hr = (*it)->SetSampleDuration(new_sample_dur));
            }
            else
                consumed_samples++;
            CHECK_HR(hr = MFCreateSample(&sample));
            CHECK_HR(hr = sample->AddBuffer(buffer));
            CHECK_HR(hr = sample->SetSampleTime(new_sample_time));
            CHECK_HR(hr = sample->SetSampleDuration(new_sample_dur));

            audio.buffer->samples.push_back(sample);

            consumed_samples_end = std::max(new_sample_time + new_sample_dur, consumed_samples_end);
        }

        // calculate timeout for the request so that it is dispatched even if there's no
        // data for it available
        presentation_clock_t clock;
        request.rp.get_clock(clock);
        if(dispatch || clock->get_current_time() >= (request.rp.request_time + request_timeout))
        {
            // update the consumed samples position
            this->consumed_samples_end = consumed_samples_end;

            // erase all consumed samples
            for(size_t i = 0; i < consumed_samples; i++)
                this->buffer.samples.pop_front();

            // pop the request from the queue
            this->requests.pop(request);

            // dispatch the request in a work queue
            if(FAILED(hr = stream->process_sample_callback->mf_put_work_item(
                stream->shared_from_this<stream_wasapi>())))
            {
                stream->rp = request_packet();
                goto done;
            }
        }
        else
            break;
    }

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
}

// TODO: the buffer of source wasapi shouldn't just grow indefinitely
void source_wasapi::process_cb(void*)
{
    HRESULT hr = S_OK;
    // nextpacketsize and frames are equal
    UINT32 nextpacketsize = 0, returned_frames = 0;
    UINT64 returned_devposition;
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
        UINT32 frames;

        // no excessive delay should happen between getbuffer and releasebuffer calls
        CHECK_HR(hr = this->audio_capture_client->GetBuffer(
            &data, &returned_frames, &flags, &returned_devposition, &first_sample_timestamp));
        getbuffer = true;
        // try fetch a next packet if no frames were returned
        // or if the frames were already returned
        if(returned_frames == 0)
        {
            getbuffer = false;
            CHECK_HR(hr = this->audio_capture_client->ReleaseBuffer(returned_frames));
            continue;
        }

        frames = returned_frames;
        devposition = returned_devposition;

        CHECK_HR(hr = MFCreateSample(&sample));

        bool silent = false;

        if(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY || 
            this->frame_base == std::numeric_limits<frame_unit>::min())
        {
            std::cout << "DATA DISCONTINUITY, " << devposition << ", " << devposition + frames << std::endl;

            presentation_time_source_t time_source = this->session->get_time_source();
            if(!time_source)
            {
                this->frame_base = std::numeric_limits<frame_unit>::min();
                goto done;
            }

            // calculate the new sample base from the timestamp
            const double frame_duration = SECOND_IN_TIME_UNIT / (double)this->samples_per_second;
            this->devposition_base = (frame_unit)devposition;
            this->frame_base = (frame_unit)
                (time_source->system_time_to_time_source((time_unit)first_sample_timestamp) /
                frame_duration);
        }
        if(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
        {
            std::cout << "TIMESTAMP ERROR" << std::endl;
        }
        if(flags & AUDCLNT_BUFFERFLAGS_SILENT)
        {
            silent = true;
            /*std::cout << "SILENT" << std::endl;*/
        }
        if(!flags)
        {
            /*std::cout << "OK, " << devposition << ", " << devposition + frames << std::endl;*/
            /*std::cout << "OK" << std::endl;*/
        }

        // convert and copy to buffer
        BYTE* buffer_data;
        const DWORD len = frames * this->block_align;
        CHECK_HR(hr = MFCreateMemoryBuffer(len, &buffer));
        CHECK_HR(hr = buffer->Lock(&buffer_data, NULL, NULL));
        if(silent)
            memset(buffer_data, 0, len);
        else
            memcpy(buffer_data, data, len);
        CHECK_HR(hr = buffer->Unlock());
        CHECK_HR(hr = buffer->SetCurrentLength(len));
        CHECK_HR(hr = sample->AddBuffer(buffer));

        // set the time and duration in frames
        const frame_unit sample_time = this->frame_base + 
            ((frame_unit)devposition - this->devposition_base);
        CHECK_HR(hr = sample->SetSampleTime(sample_time));
        CHECK_HR(hr = sample->SetSampleDuration(frames));
        this->next_frame_position = sample_time + (frame_unit)frames;

        // release buffer
        getbuffer = false;
        CHECK_HR(hr = this->audio_capture_client->ReleaseBuffer(returned_frames));

        // add sample
        this->buffer.samples.push_back(sample);
    }

done:
    if(getbuffer)
    {
        getbuffer = false;
        this->audio_capture_client->ReleaseBuffer(returned_frames);
    }
    if(FAILED(hr))
    {
        // reinit the component if the audio device has been invalidated
        if(hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING)
        {
            /*std::cout << "audio device invalidated, resetting..." << std::endl;*/

            // the reinitialization initiation is postponed to serve requests section
            // so that the process won't run at capture thread priority
            this->broken = true;

            // do not set any other variables that depend on broken here,
            // because serve requests is asynchronous
        }
        else
            throw HR_EXCEPTION(hr);
    }

    this->serve_cb(NULL);

    if(FAILED(hr = this->add_event_to_wait_queue()) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
}

HRESULT source_wasapi::play_silence()
{
    HRESULT hr = S_OK;
    UINT32 frames_padding;
    LPBYTE data;

    CHECK_HR(hr = this->audio_client_render->GetCurrentPadding(&frames_padding));
    CHECK_HR(hr = this->audio_render_client->GetBuffer(
        this->render_buffer_frame_count - frames_padding, &data));
    CHECK_HR(hr = this->audio_render_client->ReleaseBuffer(
        this->render_buffer_frame_count - frames_padding, AUDCLNT_BUFFERFLAGS_SILENT));

done:
    return hr;
}

HRESULT source_wasapi::initialize_render(IMMDevice* device, WAVEFORMATEX* engine_format)
{
    HRESULT hr = S_OK;
    LPBYTE data;

    CHECK_HR(hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, 
        (void**)&this->audio_client_render));
    CHECK_HR(hr = this->audio_client_render->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, engine_format, NULL));

    CHECK_HR(hr = this->audio_client_render->GetBufferSize(&this->render_buffer_frame_count));
    CHECK_HR(hr = this->audio_client_render->GetService(
        __uuidof(IAudioRenderClient), (void**)&this->audio_render_client));
    CHECK_HR(hr = this->audio_render_client->GetBuffer(this->render_buffer_frame_count, &data));
    CHECK_HR(hr = this->audio_render_client->ReleaseBuffer(
        this->render_buffer_frame_count, AUDCLNT_BUFFERFLAGS_SILENT));

done:
    return hr;
}

void source_wasapi::initialize(const control_pipeline2_t& ctrl_pipeline,
    const std::wstring& device_id, bool capture)
{
    HRESULT hr = S_OK;

    CComPtr<IMMDeviceEnumerator> enumerator;
    CComPtr<IMMDevice> device;
    WAVEFORMATEX* engine_format = NULL;
    UINT32 buffer_frame_count;
    REFERENCE_TIME def_device_period, min_device_period;

    this->ctrl_pipeline = ctrl_pipeline;
    this->capture = capture;

    CHECK_HR(hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator));
    CHECK_HR(hr = enumerator->GetDevice(device_id.c_str(), &device));

    CHECK_HR(hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&this->audio_client));
    CHECK_HR(hr = this->audio_client->GetMixFormat(&engine_format));

    CHECK_HR(hr = this->audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 
        this->capture ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK, 
        CAPTURE_BUFFER_DURATION, 0, engine_format, NULL));

    CHECK_HR(hr = this->audio_client->GetBufferSize(&buffer_frame_count));

    CHECK_HR(hr = this->audio_client->GetService(
        __uuidof(IAudioCaptureClient), (void**)&this->audio_capture_client));

    CHECK_HR(hr = this->audio_client->GetDevicePeriod(&def_device_period, &min_device_period));
    assert_(def_device_period < SECOND_IN_TIME_UNIT / 1000 * capture_interval_ms);

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
    this->buffer_actual_duration = (REFERENCE_TIME)
        ((double)SECOND_IN_TIME_UNIT * buffer_frame_count / this->samples_per_second);

    // initialize silence fix
    // (https://github.com/jp9000/obs-studio/blob/master/plugins/win-wasapi/win-wasapi.cpp#L199)
    if(!this->capture)
        CHECK_HR(hr = this->initialize_render(device, engine_format));

    CHECK_HR(hr = this->add_event_to_wait_queue());
    this->wait_queue = true;

    // start capturing
    if(!this->capture)
        CHECK_HR(hr = this->audio_client_render->Start());
    CHECK_HR(hr = this->audio_client->Start());

    this->started = true;

done:
    if(engine_format)
        CoTaskMemFree(engine_format);
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

media_stream_t source_wasapi::create_stream()
{
    return media_stream_t(new stream_wasapi(this->shared_from_this<source_wasapi>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_wasapi::stream_wasapi(const source_wasapi_t& source) : 
    source(source),
    audio_sample(media_buffer_samples_t(new media_buffer_samples)) 
{
    this->process_sample_callback.Attach(new async_callback_t(&stream_wasapi::process_sample_cb));
}

void stream_wasapi::process_sample_cb(void*)
{
    request_packet rp = this->rp;
    this->rp = request_packet();
    this->process_sample(this->audio_sample, rp, NULL);
}

media_stream::result_t stream_wasapi::request_sample(request_packet& rp, const media_stream*)
{
    source_wasapi::request_t request;
    request.stream = this;
    this->rp = rp;
    request.rp = this->rp;
    this->source->requests.push(request);

    return OK;
}

media_stream::result_t stream_wasapi::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream*)
{
    return this->source->session->give_sample(this, sample_view, rp, true) ? OK : FATAL_ERROR;
}