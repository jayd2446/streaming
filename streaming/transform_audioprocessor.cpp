#include <wmcodecdsp.h>
#include "transform_audioprocessor.h"
#include "transform_aac_encoder.h"
#include "source_wasapi.h"
#include "assert.h"
#include <Mferror.h>
//#include <initguid.h>
#include <iostream>
#include <limits>
EXTERN_GUID(CLSID_CResamplerMediaObject, 0xf447b69e, 0x1884, 0x4a7e, 0x80, 0x55, 0x34, 0x6f, 0x74, 0xd6, 0xed, 0xb3);

#define HALF_FILTER_LENGTH 30 /* 60 is max, but wmp and groove music uses 30 */
#define MILLISECOND_IN_TIMEUNIT (SECOND_IN_TIME_UNIT / 1000)

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#undef max
#undef min

transform_audioprocessor::transform_audioprocessor(const media_session_t& session) :
    media_source(session), 
    channels(0),
    sample_rate(0),
    block_align(0),
    running(false),
    sample_base(std::numeric_limits<frame_unit>::min()),
    next_sample_pos(0),
    next_sample_pos_in(std::numeric_limits<frame_unit>::min())
{
    /*this->serve_callback.Attach(new async_callback_t(&transform_audioprocessor::serve_cb));*/
}

void transform_audioprocessor::reset_input_type(UINT channels, UINT sample_rate, UINT bit_depth)
{
    if(channels == this->channels && sample_rate == this->sample_rate)
        return;

    // source_wasapi is always float
    if(bit_depth != (sizeof(float) * 8))
        throw std::exception();

    HRESULT hr = S_OK;

    this->channels = channels;
    this->sample_rate = sample_rate;
    this->block_align = (bit_depth * this->channels) / 8;

    this->input_type = NULL;
    CHECK_HR(hr = MFCreateMediaType(&this->input_type));
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float));
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

    mft_provides_samples =
        this->output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
    alignment = this->output_stream_info.cbAlignment;
    min_size = this->output_stream_info.cbSize;

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

void transform_audioprocessor::resample(
    media_sample_audio& out_audio, const media_sample_audio& audio, bool drain)
{
    // resampling must be serialized
    std::unique_lock<std::recursive_mutex> lock(this->resample_mutex, std::try_to_lock);
    assert_(lock.owns_lock());
    if(!lock.owns_lock())
        throw std::exception();
    // stream is assumed to have provided a buffer
    assert_(out_audio.buffer && out_audio.buffer->samples.size() == 1);

    this->reset_input_type(audio.channels, audio.sample_rate, audio.bit_depth);

    out_audio.bit_depth = sizeof(bit_depth_t) * 8;
    out_audio.channels = transform_aac_encoder::channels;
    out_audio.sample_rate = transform_aac_encoder::sample_rate;
    out_audio.frame_end = media_sample_audio::invalid_frame_end;

    // declare resampling operations
    HRESULT hr = S_OK;
    CComPtr<IMFSample> out_sample;
    CComPtr<IMFMediaBuffer> out_buffer;
    auto reset_sample = [&, this]()
    {
        HRESULT hr = S_OK;
        DWORD buflen, max_len;
        CComPtr<IMFMediaBuffer> buffer = out_buffer;

        out_sample = NULL;
        out_buffer = NULL;

        CHECK_HR(hr = buffer->GetCurrentLength(&buflen));
        CHECK_HR(hr = buffer->GetMaxLength(&max_len));

        if((buflen + this->output_stream_info.cbSize) < max_len)
        {
            CHECK_HR(hr = buffer->SetCurrentLength(max_len));
            // create a wrapper of buffer for out_buffer
            CHECK_HR(hr = MFCreateMediaBufferWrapper(
                buffer, buflen, max_len - buflen, &out_buffer));
        }
        else
        {
            // allocate a new buffer
            CHECK_HR(hr = MFCreateAlignedMemoryBuffer(
                transform_aac_encoder::input_frames * out_audio.get_block_align(),
                this->output_stream_info.cbAlignment, &out_buffer));
        }

        CHECK_HR(hr = out_buffer->SetCurrentLength(0));
        CHECK_HR(hr = MFCreateSample(&out_sample));
        CHECK_HR(hr = out_sample->AddBuffer(out_buffer));

    done:
        if(FAILED(hr))
            throw std::exception();
    };
    auto process_sample = [&, this]()
    {
        // set the new duration and timestamp for the out sample
        CComPtr<IMFMediaBuffer> buffer;
        DWORD buflen;
        HRESULT hr = S_OK;

        CHECK_HR(hr = out_sample->GetBufferByIndex(0, &buffer));
        CHECK_HR(hr = buffer->GetCurrentLength(&buflen));

        const frame_unit frame_pos = this->sample_base + this->next_sample_pos;
        const frame_unit sample_dur = buflen / out_audio.get_block_align();
        this->next_sample_pos += sample_dur;

        CHECK_HR(hr = out_sample->SetSampleTime(frame_pos));
        CHECK_HR(hr = out_sample->SetSampleDuration(sample_dur));

        const frame_unit frame_end = frame_pos + sample_dur;

        // add the out sample to buffer
        out_audio.buffer->samples.push_back(out_sample);

        // try to set the frame end
        out_audio.frame_end = std::max(out_audio.frame_end, frame_end);

    done:
        return hr;
    };
    auto drain_all = [&, this]()
    {
        std::cout << "drain on audioprocessor" << std::endl;

        HRESULT hr = S_OK;
        CHECK_HR(hr = this->processor->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
        while(reset_sample(), this->resampler_process_output(out_sample))
            CHECK_HR(hr = process_sample());

    done:
        return hr;
    };

    // get the provided buffer and clear the buffer
    CHECK_HR(hr = out_audio.buffer->samples[0]->GetBufferByIndex(0, &out_buffer));
    out_audio.buffer->samples.clear();

    // resample
    for(auto it = audio.buffer->samples.begin(); it != audio.buffer->samples.end(); it++)
    {
        // create a sample that has time and duration converted from frame unit to time unit
        CComPtr<IMFSample> sample;
        CComPtr<IMFMediaBuffer> buffer;
        frame_unit frame_pos, frame_dur;
        LONGLONG time, dur;

        CHECK_HR(hr = (*it)->GetSampleTime(&time));
        CHECK_HR(hr = (*it)->GetSampleDuration(&dur));
        CHECK_HR(hr = (*it)->GetBufferByIndex(0, &buffer));
        frame_pos = time;
        frame_dur = dur;
        time = (LONGLONG)((double)time * SECOND_IN_TIME_UNIT / audio.sample_rate);
        dur = (LONGLONG)((double)dur * SECOND_IN_TIME_UNIT / audio.sample_rate);

        CHECK_HR(hr = MFCreateSample(&sample));
        CHECK_HR(hr = sample->SetSampleTime(time));
        CHECK_HR(hr = sample->SetSampleDuration(dur));
        CHECK_HR(hr = sample->AddBuffer(buffer));

        // if the new sample has a data discontinuity flag,
        // drain the resampler before submitting new input
        if(this->next_sample_pos_in != frame_pos)
        {
            // the 'data discontinuity flag' is set
            CHECK_HR(hr = drain_all());

            const frame_unit old_base = this->sample_base;
            this->sample_base = (frame_unit)
                ((double)transform_aac_encoder::sample_rate / audio.sample_rate * frame_pos);

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

        this->next_sample_pos_in = frame_pos + frame_dur;

    back:
        hr = this->processor->ProcessInput(0, sample, 0);
        if(hr == MF_E_NOTACCEPTING)
        {
            if(reset_sample(), this->resampler_process_output(out_sample))
                CHECK_HR(hr = process_sample());

            goto back;
        }
        else
            CHECK_HR(hr);
    }

    if(drain)
        CHECK_HR(hr = drain_all());

done:
    if(FAILED(hr))
        throw std::exception();
}

void transform_audioprocessor::initialize(/*UINT32 sample_rate*/)
{
    HRESULT hr = S_OK;
    CComPtr<IWMResamplerProps> props;

    CHECK_HR(hr = CoCreateInstance(CLSID_CResamplerMediaObject, NULL, CLSCTX_INPROC_SERVER,
        __uuidof(IMFTransform), (LPVOID*)&this->processor));
    CHECK_HR(hr = this->processor->QueryInterface(&props));
    // quality
    CHECK_HR(hr = props->SetHalfFilterLength(HALF_FILTER_LENGTH));

    // set output type
    const UINT32 block_align = sizeof(bit_depth_t) * transform_aac_encoder::channels;
    CHECK_HR(hr = MFCreateMediaType(&this->output_type));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_SUBTYPE, 
        sizeof(bit_depth_t) == 4 ? MFAudioFormat_Float : MFAudioFormat_PCM));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,
        sizeof(bit_depth_t) * 8));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, transform_aac_encoder::channels));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
        transform_aac_encoder::sample_rate));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
        transform_aac_encoder::sample_rate * block_align));

    /*this->sample_rate = sample_rate;*/

done:
    if(FAILED(hr))
        throw std::exception();
}

media_stream_t transform_audioprocessor::create_stream(presentation_clock_t& clock)
{
    media_stream_clock_sink_t stream(
        new stream_audioprocessor(this->shared_from_this<transform_audioprocessor>()));
    stream->register_sink(clock);

    return stream;
}

void transform_audioprocessor::process()
{
    std::unique_lock<std::mutex> lock(this->process_mutex);

    request_t request;
    while(this->requests.pop(request))
    {
        stream_audioprocessor* stream = reinterpret_cast<stream_audioprocessor*>(request.stream);
        media_sample_audio audio(stream->audio_buffer);

        this->resample(audio, request.sample_view.audio, request.sample_view.drain);

        lock.unlock();
        this->session->give_sample(request.stream, audio, request.rp, false);
        lock.lock();
    }
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_audioprocessor::stream_audioprocessor(const transform_audioprocessor_t& transform) :
    media_stream_clock_sink(transform.get()),
    transform(transform),
    drain_point(std::numeric_limits<time_unit>::min()),
    audio_buffer(new media_buffer_samples)
{
    HRESULT hr = S_OK;

    const UINT32 block_align = 
        sizeof(transform_audioprocessor::bit_depth_t) * transform_aac_encoder::channels;
    const double frame_duration = SECOND_IN_TIME_UNIT / (double)transform_aac_encoder::sample_rate;
    const frame_unit frames =
        (frame_unit)(transform_audioprocessor::buffer_duration / frame_duration);

    CHECK_HR(hr = MFCreateSample(&this->sample));
    CHECK_HR(hr = MFCreateAlignedMemoryBuffer(
        frames * block_align * 2,
        this->transform->output_stream_info.cbAlignment, &this->buffer));
    CHECK_HR(hr = this->sample->AddBuffer(this->buffer));

done:
    if(FAILED(hr))
        throw std::exception();
}

void stream_audioprocessor::on_component_start(time_unit)
{
}

void stream_audioprocessor::on_component_stop(time_unit t)
{
    this->drain_point = t;
}

media_stream::result_t stream_audioprocessor::request_sample(request_packet& rp, const media_stream*)
{
    return this->transform->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_audioprocessor::process_sample(
    const media_sample& sample_, request_packet& rp, const media_stream*)
{
    const media_sample_audio& sample = reinterpret_cast<const media_sample_audio&>(sample_);
    HRESULT hr = S_OK;

    transform_audioprocessor::request_t request;
    request.rp = rp;
    request.stream = this;
    request.sample_view.drain = (this->drain_point == rp.request_time);
    request.sample_view.audio = sample;

    // empty the old buffer and pass the static buffer
    this->audio_buffer->samples.clear();
    CHECK_HR(hr = this->buffer->SetCurrentLength(0));
    this->audio_buffer->samples.push_back(this->sample);

    this->transform->requests.push(request);
    this->transform->process();

done:
    if(FAILED(hr))
        throw std::exception();

    return OK;
}