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