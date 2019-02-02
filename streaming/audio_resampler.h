#pragma once

#include "media_sample.h"
#include <mfapi.h>
#include <Mferror.h>
#include <vector>
#include <iostream>

#pragma comment(lib, "Mfplat.lib")

// resamples, maps channels and changes bit depth;
// audio resampler is very similar to aac encoder component

// not multithread safe
class audio_resampler
{
public:
    typedef buffer_pool<media_buffer_memory_pooled> buffer_pool_memory_t;
    // the duration of the resampled buffer that is preallocated
    // TODO: a buffer worth of second might be way too much
    static const time_unit buffer_duration = SECOND_IN_TIME_UNIT;// / 10;
private:
    bool initialized;
    UINT32 out_sample_rate, out_channels, out_bit_depth;
    UINT32 in_sample_rate, in_channels, in_bit_depth;

    CComPtr<IMFTransform> resampler;
    MFT_OUTPUT_STREAM_INFO output_stream_info;
    MFT_INPUT_STREAM_INFO input_stream_info;
    CComPtr<IMFMediaType> input_type, output_type;

    // keeps the hosts alive until the resampler has processed the input samples
    // TODO: the vector shouldn't allocate dynamic memory that much
    std::vector<media_buffer_memory_t> memory_hosts;
    std::shared_ptr<buffer_pool_memory_t> buffer_pool_memory;
    bool process_output(IMFSample* sample);
public:
    audio_resampler();
    ~audio_resampler();

    // TODO: resample only if resampling is actually needed
    void initialize(
        UINT32 out_sample_rate, UINT32 out_channels, UINT32 out_bit_depth,
        UINT32 in_sample_rate, UINT32 in_channels, UINT32 in_bit_depth);
    // drain should be used if there's a discontinuity in the original stream;
    // input parameter can be null;
    // returns the amount of frames added to container
    template<typename FrameType>
    frame_unit resample(
        frame_unit frame_start_pos,
        const FrameType&,
        media_sample_audio_frames_template<FrameType>& container,
        bool drain);
};


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

template<typename T>
frame_unit audio_resampler::resample(
    frame_unit frame_next_pos,
    const T& in,
    media_sample_audio_frames_template<T>& frames,
    bool drain)
{
    typedef T sample_t;

    const UINT32 block_align = this->out_bit_depth / 8 * this->out_channels;
    const UINT32 frames_count =
        (UINT32)convert_to_frame_unit(buffer_duration, this->out_sample_rate, 1);

    HRESULT hr = S_OK;
    frame_unit frames_added = 0;
    media_buffer_memory_t buffer;
    CComPtr<IMFSample> out_sample;
    CComPtr<IMFMediaBuffer> out_buffer;

    auto reset_sample = [&]()
    {
        HRESULT hr = S_OK;
        DWORD buflen = 0, max_len = 0;
        CComPtr<IMFMediaBuffer> mf_buffer = out_buffer;

        out_sample = NULL;
        out_buffer = NULL;

        if(mf_buffer)
        {
            CHECK_HR(hr = mf_buffer->GetCurrentLength(&buflen));
            CHECK_HR(hr = mf_buffer->GetMaxLength(&max_len));
        }

        if((buflen + this->output_stream_info.cbSize) <= max_len)
        {
            CHECK_HR(hr = mf_buffer->SetCurrentLength(max_len));
            // create a wrapper of buffer for out_buffer
            CHECK_HR(hr = MFCreateMediaBufferWrapper(
                mf_buffer, buflen, max_len - buflen, &out_buffer));
        }
        else
        {
            buffer_pool_memory_t::scoped_lock lock(this->buffer_pool_memory->mutex);
            buffer = this->buffer_pool_memory->acquire_buffer();
            buffer->initialize(frames_count * block_align);
            out_buffer = buffer->buffer;
        }

        CHECK_HR(hr = out_buffer->SetCurrentLength(0));
        CHECK_HR(hr = MFCreateSample(&out_sample));
        CHECK_HR(hr = out_sample->AddBuffer(out_buffer));

    done:
        if(FAILED(hr))
            throw HR_EXCEPTION(hr);
    };
    auto process_sample = [&]()
    {
        // set the new duration and timestamp for the out sample
        CComPtr<IMFMediaBuffer> mf_buffer;
        DWORD buflen;
        HRESULT hr = S_OK;
        sample_t consec_frames;

        CHECK_HR(hr = out_sample->GetBufferByIndex(0, &mf_buffer));
        CHECK_HR(hr = mf_buffer->GetCurrentLength(&buflen));

        const frame_unit frame_pos = frame_next_pos;
        const frame_unit frame_dur = buflen / block_align;

        consec_frames.memory_host = buffer;
        consec_frames.pos = frame_pos;
        consec_frames.dur = frame_dur;
        consec_frames.buffer = mf_buffer;

        frames.end = std::max(frames.end, consec_frames.pos + consec_frames.dur);
        frames.frames.push_back(std::move(consec_frames));

        frame_next_pos += frame_dur;
        frames_added += frame_dur;

        // release the memory hosts
        this->memory_hosts.clear();

    done:
        return hr;
    };
    auto drain_all = [&]()
    {
        std::cout << "drain on audio resampler" << std::endl;

        HRESULT hr = S_OK;
        CHECK_HR(hr = this->resampler->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
        while(reset_sample(), this->process_output(out_sample))
            CHECK_HR(hr = process_sample());

    done:
        return hr;
    };

    /*CHECK_HR(hr = MFCreateSample(&out_sample));
    out_buffer = buffer->buffer;
    CHECK_HR(hr = out_sample->AddBuffer(out_buffer));*/

    // resample
    if(in.buffer)
    {
        // create a sample that has time and duration converted from frame unit to time unit
        CComPtr<IMFSample> in_sample;
        CComPtr<IMFMediaBuffer> in_buffer;
        /*CComPtr<IUnknown> lifetime_tracker;*/
        /*frame_unit frame_pos, frame_dur;
        LONGLONG time, dur;*/

        CHECK_HR(hr = MFCreateSample(&in_sample));
        in_buffer = in.buffer;
        CHECK_HR(hr = in_sample->AddBuffer(in_buffer));
        /*lifetime_tracker = create_lifetime_tracker(in.memory_host);
        CHECK_HR(hr = in_sample->SetUnknown(media_sample_lifetime_tracker_guid,
            lifetime_tracker));*/

            /*frame_pos = in.pos;
            frame_dur = in.dur;
            time = (LONGLONG)convert_to_time_unit(frame_pos, this->in_sample_rate, 1);
            dur = (LONGLONG)convert_to_time_unit(frame_dur, this->in_sample_rate, 1);*/

            // TODO: should be probably in_sample
            /*CHECK_HR(hr = out_sample->SetSampleTime(time));
            CHECK_HR(hr = out_sample->SetSampleDuration(dur));*/

            /*if(drain)
                CHECK_HR(hr = drain_all());*/

    back:
        hr = this->resampler->ProcessInput(0, in_sample, 0);
        if(hr == MF_E_NOTACCEPTING)
        {
            if(reset_sample(), this->process_output(out_sample))
                CHECK_HR(hr = process_sample());

            goto back;
        }
        else
        {
            CHECK_HR(hr);
            this->memory_hosts.push_back(in.memory_host);
        }
    }

    if(drain)
        CHECK_HR(hr = drain_all());

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);

    return frames_added;
}

#undef CHECK_HR