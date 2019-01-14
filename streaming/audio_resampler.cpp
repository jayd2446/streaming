#include <wmcodecdsp.h>
#include "audio_resampler.h"
#include "transform_aac_encoder.h"
#include "assert.h"
#include <Mferror.h>
#include <iostream>
#include <limits>

EXTERN_GUID(CLSID_CResamplerMediaObject, 0xf447b69e, 0x1884, 0x4a7e, 0x80, 0x55, 0x34, 0x6f, 0x74, 0xd6, 0xed, 0xb3);

#define HALF_FILTER_LENGTH 30 /* 60 is max, but wmp and groove music uses 30 */

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
#undef max
#undef min

audio_resampler::audio_resampler() : 
    buffer_pool_memory(new buffer_pool_memory_t),
    initialized(false)
{
}

audio_resampler::~audio_resampler()
{
    buffer_pool_memory_t::scoped_lock lock(this->buffer_pool_memory->mutex);
    this->buffer_pool_memory->dispose();
}

bool audio_resampler::process_output(IMFSample* sample)
{
    HRESULT hr = S_OK;

    DWORD mft_provides_samples;
    DWORD alignment;
    DWORD min_size, buflen;
    MFT_OUTPUT_DATA_BUFFER output;
    DWORD status = 0;
    CComPtr<IMFMediaBuffer> buffer;

    mft_provides_samples =
        this->output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
    alignment = this->output_stream_info.cbAlignment;
    min_size = this->output_stream_info.cbSize;

    CHECK_HR(hr = sample->GetBufferByIndex(0, &buffer));
    CHECK_HR(hr = buffer->SetCurrentLength(0));
    CHECK_HR(hr = buffer->GetMaxLength(&buflen));

    if(mft_provides_samples)
        throw HR_EXCEPTION(hr);
    if(buflen < min_size)
        throw HR_EXCEPTION(hr);
    if(buflen % alignment != 0)
        throw HR_EXCEPTION(hr);

    output.dwStreamID = 0;
    output.dwStatus = 0;
    output.pEvents = NULL;
    output.pSample = sample;

    CHECK_HR(hr = this->resampler->ProcessOutput(0, 1, &output, &status));

done:
    if(hr != MF_E_TRANSFORM_NEED_MORE_INPUT && FAILED(hr))
        throw HR_EXCEPTION(hr);
    return SUCCEEDED(hr);
}

void audio_resampler::initialize(
    UINT32 out_sample_rate, UINT32 out_channels, UINT32 out_bit_depth,
    UINT32 in_sample_rate, UINT32 in_channels, UINT32 in_bit_depth)
{
    if(this->initialized)
        throw HR_EXCEPTION(E_UNEXPECTED);

    this->initialized = true;
    this->out_sample_rate = out_sample_rate;
    this->out_channels = out_channels;
    this->out_bit_depth = out_bit_depth;
    this->in_sample_rate = in_sample_rate;
    this->in_channels = in_channels;
    this->in_bit_depth = in_bit_depth;

    HRESULT hr = S_OK;
    CComPtr<IWMResamplerProps> props;

    CHECK_HR(hr = CoCreateInstance(CLSID_CResamplerMediaObject, NULL, CLSCTX_INPROC_SERVER,
        __uuidof(IMFTransform), (LPVOID*)&this->resampler));
    CHECK_HR(hr = this->resampler->QueryInterface(&props));

    // quality
    CHECK_HR(hr = props->SetHalfFilterLength(HALF_FILTER_LENGTH));

    // set output type
    const UINT32 out_block_align = this->out_bit_depth / 8 * this->out_channels;
    CHECK_HR(hr = MFCreateMediaType(&this->output_type));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_SUBTYPE,
        this->out_bit_depth == 32 ? MFAudioFormat_Float : MFAudioFormat_PCM));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, this->out_bit_depth));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, this->out_channels));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, this->out_sample_rate));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, out_block_align));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
        this->out_sample_rate * out_block_align));

    // set input type
    const UINT32 in_block_align = this->in_bit_depth / 8 * this->in_channels;
    CHECK_HR(hr = MFCreateMediaType(&this->input_type));
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_SUBTYPE,
        this->in_bit_depth == 32 ? MFAudioFormat_Float : MFAudioFormat_PCM));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, this->in_bit_depth));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, this->in_channels));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, this->in_sample_rate));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, in_block_align));
    CHECK_HR(hr = this->input_type->SetUINT32(
        MF_MT_AUDIO_AVG_BYTES_PER_SECOND, this->in_sample_rate * in_block_align));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));

    CHECK_HR(hr = this->resampler->SetInputType(0, this->input_type, 0));
    CHECK_HR(hr = this->resampler->SetOutputType(0, this->output_type, 0));
    CHECK_HR(hr = this->resampler->GetOutputStreamInfo(0, &this->output_stream_info));
    CHECK_HR(hr = this->resampler->GetInputStreamInfo(0, &this->input_stream_info));

    CHECK_HR(hr = this->resampler->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL));
    CHECK_HR(hr = this->resampler->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
    CHECK_HR(hr = this->resampler->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

frame_unit audio_resampler::resample(frame_unit frame_next_pos,
    const media_sample_audio_consecutive_frames& in, 
    media_sample_audio_frames& frames,
    bool drain)
{
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
        media_sample_audio_consecutive_frames consec_frames;

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