#include "transform_audioprocessor.h"
#include "transform_aac_encoder.h"
#include <Mferror.h>
#include <initguid.h>
#include <wmcodecdsp.h>
#include <iostream>
EXTERN_GUID(CLSID_CResamplerMediaObject, 0xf447b69e, 0x1884, 0x4a7e, 0x80, 0x55, 0x34, 0x6f, 0x74, 0xd6, 0xed, 0xb3);

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#undef max
#undef min

transform_audioprocessor::transform_audioprocessor(const media_session_t& session) :
    media_source(session), channels(0), sample_rate(0), block_align(0), running(false)
{
}

void transform_audioprocessor::reset_input_type(UINT channels, UINT sample_rate)
{
    scoped_lock(this->set_type_mutex);

    if(channels == this->channels && sample_rate == this->sample_rate)
        return;

    HRESULT hr = S_OK;

    this->channels = channels;
    this->sample_rate = sample_rate;
    this->block_align = sizeof(transform_aac_encoder::bit_depth_t) * this->channels;

    this->input_type = NULL;
    CHECK_HR(hr = MFCreateMediaType(&this->input_type));
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 
        sizeof(transform_aac_encoder::bit_depth_t) * 8));
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

void transform_audioprocessor::initialize()
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
}

void stream_audioprocessor::processing_cb(void*)
{
    media_buffer_samples_t samples = this->pending_packet.sample_view->get_buffer<media_buffer_samples>();
    media_buffer_samples_t output_samples_buffer(new media_buffer_samples);
    media_sample_view_t sample_view(new media_sample_view(output_samples_buffer));
    request_packet rp;

    // send input data to audio processor
    HRESULT hr = S_OK;

    CComPtr<IMFSample> out_sample;
    CComPtr<IMFMediaBuffer> out_buffer;
    auto reset_sample = [&out_sample, &out_buffer, this]()
    {
        HRESULT hr = S_OK;
        CHECK_HR(hr = MFCreateSample(&out_sample));
        CHECK_HR(hr = MFCreateAlignedMemoryBuffer(
            OUT_BUFFER_FRAMES * this->transform->block_align,
            this->transform->output_stream_info.cbAlignment, &out_buffer));
        CHECK_HR(hr = out_sample->AddBuffer(out_buffer));

    done:
        if(FAILED(hr))
            throw std::exception();
    };

    reset_sample();
    LONGLONG next_sample_time = std::numeric_limits<LONGLONG>::min(), last_sample_time;
    CHECK_HR(hr = (*samples->samples.begin())->GetSampleTime(&last_sample_time));
    for(auto it = samples->samples.begin(); it != samples->samples.end(); it++)
    {
        // create a sample that has time and duration converted from frame unit to time unit
        CComPtr<IMFSample> sample;
        CComPtr<IMFMediaBuffer> buffer;
        LONGLONG time, dur;

        CHECK_HR(hr = (*it)->GetSampleTime(&time));
        CHECK_HR(hr = (*it)->GetSampleDuration(&dur));
        CHECK_HR(hr = (*it)->GetBufferByIndex(0, &buffer));
        time = (LONGLONG)((double)time * SECOND_IN_TIME_UNIT / samples->sample_rate);
        dur = (LONGLONG)((double)dur * SECOND_IN_TIME_UNIT / samples->sample_rate);

        CHECK_HR(hr = MFCreateSample(&sample));
        CHECK_HR(hr = sample->SetSampleTime(time));
        CHECK_HR(hr = sample->SetSampleDuration(dur));
        CHECK_HR(hr = sample->AddBuffer(buffer));

    back:
        hr = this->transform->processor->ProcessInput(0, sample, 0);
        if(hr == MF_E_NOTACCEPTING)
        {
            if(!this->process_output(out_sample))
                goto back;

            LONGLONG sample_time, sample_dur, calculated_time;
            /*CHECK_HR(hr = (*it)->GetSampleTime(&sample_time));*/

            // convert the time and duration to converted sample rate
            CComPtr<IMFMediaBuffer> buffer;
            DWORD buflen;
            HRESULT hr = S_OK;
            CHECK_HR(hr = out_sample->GetBufferByIndex(0, &buffer));
            CHECK_HR(hr = buffer->GetCurrentLength(&buflen));

            calculated_time = (LONGLONG)
                ((double)transform_aac_encoder::sample_rate / samples->sample_rate * last_sample_time);
            sample_time = std::max(next_sample_time, calculated_time);
            sample_dur = buflen / transform_aac_encoder::block_align;
            next_sample_time = sample_time + sample_dur;
            std::cout << sample_time << " " << next_sample_time << std::endl;
                
            CHECK_HR(hr = out_sample->SetSampleTime(sample_time));
            CHECK_HR(hr = out_sample->SetSampleDuration(sample_dur));

            // add the out_sample to sample view
            output_samples_buffer->samples.push_back(out_sample);

            // reset the out sample
            out_sample = NULL;
            out_buffer = NULL;
            reset_sample();

            CHECK_HR(hr = (*it)->GetSampleTime(&last_sample_time));
            goto back;
        }
        else
            CHECK_HR(hr);
    }

    // drain the processor
    /*CHECK_HR(hr = this->transform->processor->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, NULL));*/
    while(this->process_output(out_sample))
    {
        LONGLONG sample_time, sample_dur;

        // convert the time and duration to converted sample rate
        CComPtr<IMFMediaBuffer> buffer;
        DWORD buflen;
        HRESULT hr = S_OK;
        CHECK_HR(hr = out_sample->GetBufferByIndex(0, &buffer));
        CHECK_HR(hr = buffer->GetCurrentLength(&buflen));

        sample_time = next_sample_time;
        sample_dur = buflen / transform_aac_encoder::block_align;
        next_sample_time = sample_time + sample_dur;
                
        CHECK_HR(hr = out_sample->SetSampleTime(sample_time));
        CHECK_HR(hr = out_sample->SetSampleDuration(sample_dur));

        // add the out_sample to sample view
        output_samples_buffer->samples.push_back(out_sample);

        // reset the out sample
        out_sample = NULL;
        out_buffer = NULL;
        reset_sample();
    }

    std::cout << next_sample_time << std::endl;
    sample_view->sample.timestamp = this->pending_packet.sample_view->sample.timestamp;
    output_samples_buffer->channels = transform_aac_encoder::channels;
    output_samples_buffer->sample_rate = transform_aac_encoder::sample_rate;
    output_samples_buffer->bit_depth = sizeof(transform_aac_encoder::bit_depth_t);
    rp = this->pending_packet.rp;

    this->pending_packet.sample_view = NULL;
    this->pending_packet.rp = request_packet();

    this->transform->session->give_sample(this, sample_view, rp, false);
done:
    if(FAILED(hr))
        throw std::exception();
}

bool stream_audioprocessor::process_output(IMFSample* sample)
{
    HRESULT hr = S_OK;

    DWORD mft_provides_samples;
    DWORD alignment;
    DWORD min_size, buflen;
    MFT_OUTPUT_DATA_BUFFER output;
    DWORD status = 0;
    CComPtr<IMFMediaBuffer> buffer;

    {
        scoped_lock lock(this->transform->set_type_mutex);
        mft_provides_samples =
            this->transform->output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
        alignment = this->transform->output_stream_info.cbAlignment;
        min_size = this->transform->output_stream_info.cbSize;
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
    
    CHECK_HR(hr = this->transform->processor->ProcessOutput(0, 1, &output, &status));

done:
    if(hr != MF_E_TRANSFORM_NEED_MORE_INPUT && FAILED(hr))
        throw std::exception();
    return SUCCEEDED(hr);
}

media_stream::result_t stream_audioprocessor::request_sample(request_packet& rp, const media_stream*)
{
    return this->transform->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_audioprocessor::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    if(sample_view)
    {
        media_buffer_samples_t samples = sample_view->get_buffer<media_buffer_samples>();

        if(samples->channels != transform_aac_encoder::channels || 
            samples->sample_rate != transform_aac_encoder::sample_rate)
        {
            this->transform->reset_input_type(samples->channels, samples->sample_rate);
            this->pending_packet.rp = rp;
            this->pending_packet.sample_view = sample_view;

            // TODO: audio processing should be dispatched to a work item
            this->processing_cb(NULL);
            return OK;
        }
    }
        
    this->transform->session->give_sample(this, sample_view, rp, false);
    return OK;
}