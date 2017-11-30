#include <wmcodecdsp.h>
#include "transform_audioprocessor.h"
#include "transform_aac_encoder.h"
#include "source_loopback.h"
#include "assert.h"
#include <Mferror.h>
//#include <initguid.h>
#include <iostream>
EXTERN_GUID(CLSID_CResamplerMediaObject, 0xf447b69e, 0x1884, 0x4a7e, 0x80, 0x55, 0x34, 0x6f, 0x74, 0xd6, 0xed, 0xb3);

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#undef max
#undef min

transform_audioprocessor::transform_audioprocessor(const media_session_t& session) :
    media_source(session), channels(/*transform_aac_encoder::channels*/0), 
    sample_rate(/*transform_aac_encoder::sample_rate*/0), 
    block_align(/*transform_aac_encoder::block_align*/0), 
    running(false),
    sample_base(std::numeric_limits<frame_unit>::min()),
    next_sample_pos(0),
    consumed_samples_end(std::numeric_limits<frame_unit>::min())
{
    this->serve_callback.Attach(new async_callback_t(&transform_audioprocessor::serve_cb));
}

void transform_audioprocessor::reset_input_type(UINT channels, UINT sample_rate, UINT bit_depth)
{
    scoped_lock(this->set_type_mutex);

    if(channels == this->channels && sample_rate == this->sample_rate)
        return;

    HRESULT hr = S_OK;

    this->channels = channels;
    this->sample_rate = sample_rate;
    this->block_align = (bit_depth * this->channels) / 8;

    this->input_type = NULL;
    CHECK_HR(hr = MFCreateMediaType(&this->input_type));
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bit_depth));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, this->channels));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, this->sample_rate));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, this->block_align));
    CHECK_HR(hr = this->input_type->SetUINT32(
        MF_MT_AUDIO_AVG_BYTES_PER_SECOND, this->sample_rate * this->block_align));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));

    CHECK_HR(hr = this->processor->SetInputType(0, this->input_type, 0));
    CHECK_HR(hr = this->processor->SetOutputType(0, this->output_type, 0));
    CHECK_HR(hr = this->processor->GetOutputStreamInfo(0, &this->output_stream_info));

    CHECK_HR(hr = this->processor->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL));
    if(!this->running)
    {
        CHECK_HR(hr = this->processor->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
        CHECK_HR(hr = this->processor->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));
        this->running = true;
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

bool transform_audioprocessor::resampler_process_output(IMFSample* sample)
{
    HRESULT hr = S_OK;

    DWORD mft_provides_samples;
    DWORD alignment;
    DWORD min_size, buflen;
    MFT_OUTPUT_DATA_BUFFER output;
    DWORD status = 0;
    CComPtr<IMFMediaBuffer> buffer;

    {
        scoped_lock lock(this->set_type_mutex);
        mft_provides_samples =
            this->output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
        alignment = this->output_stream_info.cbAlignment;
        min_size = this->output_stream_info.cbSize;
    }

    CHECK_HR(hr = sample->GetBufferByIndex(0, &buffer));
    CHECK_HR(hr = buffer->SetCurrentLength(0));
    CHECK_HR(hr = buffer->GetMaxLength(&buflen));

    if(mft_provides_samples)
        throw std::exception();
    if(buflen < min_size)
        throw std::exception();
    if(buflen % alignment != 0)
        throw std::exception();

    output.dwStreamID = 0;
    output.dwStatus = 0;
    output.pEvents = NULL;
    output.pSample = sample;
    
    CHECK_HR(hr = this->processor->ProcessOutput(0, 1, &output, &status));

done:
    if(hr != MF_E_TRANSFORM_NEED_MORE_INPUT && FAILED(hr))
        throw std::exception();
    return SUCCEEDED(hr);
}

void transform_audioprocessor::resample(const media_buffer_samples& samples)
{
    this->reset_input_type(samples.channels, samples.sample_rate, samples.bit_depth);

    // TODO: resampling should be skipped;
    // this means that only the sample bases should be set

    //// do not resample if the samples already have valid properties
    //if(samples.bit_depth == sizeof(transform_aac_encoder::bit_depth_t) &&
    //    samples.channels == transform_aac_encoder::channels &&
    //    samples.sample_rate == transform_aac_encoder::sample_rate)
    //{
    //    scoped_lock lock(this->samples_mutex);
    //    // TODO: rather inefficient
    //    for(auto it = samples.samples.begin(); it != samples.samples.end(); it++)
    //        this->samples.push_back(*it);
    //    return;
    //}

    // resample
    HRESULT hr = S_OK;
    CComPtr<IMFSample> out_sample;
    CComPtr<IMFMediaBuffer> out_buffer;
    auto reset_sample = [&out_sample, &out_buffer, this]()
    {
        HRESULT hr = S_OK;
        CHECK_HR(hr = MFCreateSample(&out_sample));
        CHECK_HR(hr = MFCreateAlignedMemoryBuffer(
            OUT_BUFFER_FRAMES * this->block_align,
            this->output_stream_info.cbAlignment, &out_buffer));
        CHECK_HR(hr = out_sample->AddBuffer(out_buffer));

    done:
        if(FAILED(hr))
            throw std::exception();
    };
    auto process_sample = [&out_sample, &out_buffer, &reset_sample, this]()
    {
        // set the new duration and timestamp for the out sample
        CComPtr<IMFMediaBuffer> buffer;
        DWORD buflen;
        HRESULT hr = S_OK;
        CHECK_HR(hr = out_sample->GetBufferByIndex(0, &buffer));
        CHECK_HR(hr = buffer->GetCurrentLength(&buflen));

        const frame_unit frame_pos = this->sample_base + this->next_sample_pos;
        const frame_unit sample_dur = buflen / transform_aac_encoder::block_align;
        this->next_sample_pos += sample_dur;

        CHECK_HR(hr = out_sample->SetSampleTime(frame_pos));
        CHECK_HR(hr = out_sample->SetSampleDuration(sample_dur));

        // add the out sample to samples container
        {
            scoped_lock lock(this->samples_mutex);
            this->samples.push_back(out_sample);
        }

        // reset the out sample
        out_sample = NULL;
        out_buffer = NULL;
        reset_sample();

    done:
        if(FAILED(hr))
            throw std::exception();
    };

    reset_sample();
    for(auto it = samples.samples.begin(); it != samples.samples.end(); it++)
    {
        // create a sample that has time and duration converted from frame unit to time unit
        CComPtr<IMFSample> sample;
        CComPtr<IMFMediaBuffer> buffer;
        LONGLONG time, dur;
        source_loopback::sample_base_t sample_base;

        CHECK_HR(hr = (*it)->GetSampleTime(&time));
        CHECK_HR(hr = (*it)->GetSampleDuration(&dur));
        CHECK_HR(hr = (*it)->GetBufferByIndex(0, &buffer));
        time = (LONGLONG)((double)time * SECOND_IN_TIME_UNIT / samples.sample_rate);
        dur = (LONGLONG)((double)dur * SECOND_IN_TIME_UNIT / samples.sample_rate);

        CHECK_HR(hr = MFCreateSample(&sample));
        CHECK_HR(hr = sample->SetSampleTime(time));
        CHECK_HR(hr = sample->SetSampleDuration(dur));
        CHECK_HR(hr = sample->AddBuffer(buffer));

        // if the new sample has a data discontinuity flag,
        // drain the resampler before submitting new input
        CHECK_HR(hr = (*it)->GetBlob(MF_MT_USER_DATA, (UINT8*)&sample_base, sizeof(sample_base), NULL));
        if(sample_base.sample >= 0)
        {
            // the 'data discontinuity flag' is set
            if(this->resampler_process_output(out_sample))
                process_sample();

            // set the new base
            const double sample_duration = SECOND_IN_TIME_UNIT / (double)samples.sample_rate;
            const frame_unit old_base = this->sample_base;
            presentation_time_source_t time_source = this->session->get_time_source();
            frame_unit sample_time = (frame_unit)
                (time_source->system_time_to_time_source(sample_base.time) / sample_duration);
            this->sample_base = (frame_unit)
                ((double)transform_aac_encoder::sample_rate / samples.sample_rate * sample_time);

            if((old_base + this->next_sample_pos) > this->sample_base)
            {
                const frame_unit base_drift = old_base + this->next_sample_pos - this->sample_base;
                std::cout << "SAMPLE BASE DRIFT: " << base_drift << " frames, ";
                std::cout << 
                    (double)MILLISECOND_IN_TIMEUNIT / transform_aac_encoder::sample_rate * base_drift 
                    << "ms" << std::endl;
            }

            this->next_sample_pos = 0;
        }

    back:
        hr = this->processor->ProcessInput(0, sample, 0);
        if(hr == MF_E_NOTACCEPTING)
        {
            if(!this->resampler_process_output(out_sample))
                goto back;

            process_sample();
            goto back;
        }
        else 
            CHECK_HR(hr);
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

void transform_audioprocessor::try_serve(const media_buffer_samples& samples)
{
    // resample the samples
    if(!samples.samples.empty())
        this->resample(samples);

    // try to serve
    HRESULT hr = S_OK;
    request_t request;
    while(this->requests.get(request))
    {
        // clock is assumed to be valid if there's a request pending
        presentation_clock_t clock;
        request.rp.get_clock(clock);

        // samples are collected up to the request time;
        // sample that goes over the request time will not be collected

        bool dispatch = false;
        media_buffer_samples_t samples(new media_buffer_samples);
        const double sample_duration = SECOND_IN_TIME_UNIT / (double)transform_aac_encoder::sample_rate;
        const frame_unit request_end = (frame_unit)(request.rp.request_time / sample_duration);

        std::unique_lock<std::recursive_mutex> lock2(this->samples_mutex);
        size_t consumed_samples = 0;
        frame_unit consumed_samples_end = this->consumed_samples_end;
        for(auto it = this->samples.begin(); it != this->samples.end() && !dispatch; it++)
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
            const DWORD offset_end  = (DWORD)frame_diff_end * transform_aac_encoder::block_align;
            const frame_unit new_sample_time = sample_pos;
            const frame_unit new_sample_dur = sample_dur - frame_diff_end;

            // discard frames that have older time stamp than the last consumed one
            if(new_sample_time >= consumed_samples_end)
            {
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
                    const LONGLONG new_sample_dur = offset_end / transform_aac_encoder::block_align;
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

                // TODO: define this flag in audio processor
                // because of data discontinuity the new sample base might yield sample times
                // that 'travel back in time', so keep track of last consumed sample and discard
                // samples that are older than that;
                // the audio might become out of sync by the amount of discarded frames
                // (actually it won't)
                if(!(request.rp.flags & AUDIO_DISCARD_PREVIOUS_SAMPLES))
                    // TODO: pushing samples to a container could be deferred to
                    // dispatching block
                    samples->samples.push_back(sample);
            }
            else
            {
                std::cout << "discarded" << std::endl;
                consumed_samples++;
            }

            consumed_samples_end = std::max(new_sample_time + new_sample_dur, consumed_samples_end);
            /*if(dispatch)
                this->consumed_samples_end = 
                std::max(new_sample_time + new_sample_dur, this->consumed_samples_end);*/
        }

        // TODO: stale requests should be checked other way
        // request is considered stale if the device time has already passed the request time
        // and there isn't any samples to serve
        const time_unit current_time = clock->get_current_time();
        if(request.rp.request_time <= (current_time - SECOND_IN_TIME_UNIT * 2))
            dispatch = true;

        if(dispatch)
        {
            // update the consumed samples position
            this->consumed_samples_end = consumed_samples_end;

            // erase all consumed samples
            for(size_t i = 0; i < consumed_samples; i++)
                this->samples.pop_front();

            // pop the request from the queue
            this->requests.pop(request);

            lock2.unlock();
            /*lock.unlock();*/
            // dispatch the request
            media_sample_view_t sample_view;
            if(!samples->samples.empty())
            {
                sample_view.reset(new media_sample_view(samples));
                samples->bit_depth = sizeof(transform_aac_encoder::bit_depth_t) * 8;
                samples->channels = transform_aac_encoder::channels;
                samples->sample_rate = transform_aac_encoder::sample_rate;
            }
            this->session->give_sample(request.stream, sample_view, request.rp, false);
            /*lock.lock();*/
        }
        else
            break;
    }

done:
    if(FAILED(hr))
        throw std::exception();
}

void transform_audioprocessor::serve_cb(void*)
{
    // only one thread should be processing the requests so that resampling and cut operations
    // stay consistent
    std::unique_lock<std::recursive_mutex> lock(this->process_mutex, std::try_to_lock);
    if(!lock.owns_lock())
        return;

    media_buffer_samples samples;
    this->audio_device->swap_buffers(samples);
    this->try_serve(samples);
}

void transform_audioprocessor::initialize(source_loopback* audio_device)
{
    HRESULT hr = S_OK;
    CComPtr<IWMResamplerProps> props;

    CHECK_HR(hr = CoCreateInstance(CLSID_CResamplerMediaObject, NULL, CLSCTX_INPROC_SERVER, 
        __uuidof(IMFTransform), (LPVOID*)&this->processor));
    CHECK_HR(hr = this->processor->QueryInterface(&props));
    // best quality
    CHECK_HR(hr = props->SetHalfFilterLength(60));

    // set output type
    CHECK_HR(hr = MFCreateMediaType(&this->output_type));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 
        sizeof(transform_aac_encoder::bit_depth_t) * 8));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, transform_aac_encoder::channels));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 
        transform_aac_encoder::sample_rate));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 
        transform_aac_encoder::block_align));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 
        transform_aac_encoder::sample_rate * transform_aac_encoder::block_align));

    this->serve_callback->set_callback(this->shared_from_this<transform_audioprocessor>());
    this->audio_device = audio_device;
    this->audio_device->serve_callback = this->serve_callback;

done:
    if(FAILED(hr))
        throw std::exception();
}

media_stream_t transform_audioprocessor::create_stream()
{
    return media_stream_t(new stream_audioprocessor(this->shared_from_this<transform_audioprocessor>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_audioprocessor::stream_audioprocessor(const transform_audioprocessor_t& transform) :
    transform(transform)
{
    this->process_callback.Attach(new async_callback_t(&stream_audioprocessor::processing_cb));
}

void stream_audioprocessor::processing_cb(void*)
{
    /*this->transform->try_serve();*/
}

media_stream::result_t stream_audioprocessor::request_sample(request_packet& rp, const media_stream*)
{
    transform_audioprocessor::request_t request;
    request.stream = this;
    request.rp = rp;
    this->transform->requests.push(request);
    return OK;

    /*return this->transform->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;*/
}

media_stream::result_t stream_audioprocessor::process_sample(
    const media_sample_view_t&, request_packet&, const media_stream*)
{
    assert_(false);

    /*HRESULT hr = S_OK;
    transform_audioprocessor::request_t request;
    request.stream = this;
    request.rp = rp;

    this->transform->requests.push(request);
    request.sample_view = sample_view;
    this->transform->requests_resample.push(request);
    CHECK_HR(hr = 
        this->process_callback->mf_put_work_item(this->shared_from_this<stream_audioprocessor>()));

done:
    if(FAILED(hr))
        throw std::exception();*/

    return OK;
}