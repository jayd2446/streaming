#include "transform_aac_encoder.h"
#include <Mferror.h>
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
#undef min
#undef max

transform_aac_encoder::transform_aac_encoder(const media_session_t& session) : 
    media_source(session),
    last_time_stamp(std::numeric_limits<frame_unit>::min()),
    time_shift(-1)
{
}

void transform_aac_encoder::processing_cb(void*)
{
    HRESULT hr = S_OK;

    std::unique_lock<std::recursive_mutex> lock(this->encoder_mutex);
    request_t request;

    while(this->requests.pop(request))
    {
        media_sample_audio& audio = request.sample_view.audio;
        media_sample_aac out_audio(media_buffer_samples_t(new media_buffer_samples));
        out_audio.bit_depth = audio.bit_depth;
        out_audio.channels = audio.channels;
        out_audio.sample_rate = audio.sample_rate;

        const double sample_duration = SECOND_IN_TIME_UNIT / (double)audio.sample_rate;

        assert_(audio.bit_depth == (sizeof(bit_depth_t) * 8) &&
            audio.channels == channels &&
            audio.sample_rate == sample_rate);

        CComPtr<IMFSample> out_sample;
        CComPtr<IMFMediaBuffer> out_buffer;
        auto reset_sample = [&out_sample, &out_buffer, this]()
        {
            HRESULT hr = S_OK;
            CHECK_HR(hr = MFCreateSample(&out_sample));
            CHECK_HR(hr = MFCreateMemoryBuffer(
                this->output_stream_info.cbSize, &out_buffer));
            CHECK_HR(hr = out_sample->AddBuffer(out_buffer));

        done:
            return hr;
        };
        auto process_sample = [&out_sample, &out_buffer, &reset_sample, &out_audio, this]()
        {
            HRESULT hr = S_OK;

            out_audio.buffer->samples.push_back(out_sample);
            out_sample = NULL;
            out_buffer = NULL;

            CHECK_HR(hr = reset_sample());
        done:
            return hr;
        };
        auto drain_all = [&]()
        {
            std::cout << "drain on aac encoder" << std::endl;

            HRESULT hr = S_OK;
            CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
            while(this->process_output(out_sample))
                CHECK_HR(hr = process_sample());

        done:
            return hr;
        };

        CHECK_HR(hr = reset_sample());
        for(auto it = audio.buffer->samples.begin(); it != audio.buffer->samples.end(); it++)
        {
            // convert the frame units to time units
            frame_unit ts, dur;
            CHECK_HR(hr = (*it)->GetSampleTime(&ts));
            CHECK_HR(hr = (*it)->GetSampleDuration(&dur));

            if(ts < this->last_time_stamp)
            {
                std::cout << "timestamp error in transform_aac_encoder::processing_cb" << std::endl;
                assert_(false);
            }
            this->last_time_stamp = ts + dur;

            ts = (time_unit)(ts * sample_duration) - this->time_shift;
            dur = (time_unit)(dur * sample_duration);
            if(ts < 0)
            {
                time_unit off = ts;
                ts = 0;
                std::cout << "aac encoder time shift was off by " << off << std::endl;
            }
            CHECK_HR(hr = (*it)->SetSampleTime(ts));
            CHECK_HR(hr = (*it)->SetSampleDuration(dur));
               
            /*std::cout << "ts: " << ts << ", dur+ts: " << ts + dur << std::endl;*/

        back:
            hr = this->encoder->ProcessInput(this->input_id, *it, 0);
            if(hr == MF_E_NOTACCEPTING)
            {
                if(this->process_output(out_sample))
                    CHECK_HR(hr = process_sample());

                goto back;
            }
            else
                CHECK_HR(hr)
        }

        if(request.sample_view.drain)
            CHECK_HR(hr = drain_all());

        lock.unlock();
        this->session->give_sample(request.stream, out_audio, request.rp, false);
        lock.lock();
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

bool transform_aac_encoder::process_output(IMFSample* sample)
{
    HRESULT hr = S_OK;

    const DWORD mft_provides_samples =
        this->output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;

    MFT_OUTPUT_DATA_BUFFER output;
    DWORD status = 0;

    if(mft_provides_samples)
        throw HR_EXCEPTION(hr);
    if(this->output_stream_info.cbAlignment)
        throw HR_EXCEPTION(hr);

    output.dwStreamID = this->output_id;
    output.dwStatus = 0;
    output.pEvents = NULL;
    output.pSample = sample;

    CHECK_HR(hr = this->encoder->ProcessOutput(0, 1, &output, &status));

done:
    if(hr != MF_E_TRANSFORM_NEED_MORE_INPUT && FAILED(hr))
        throw HR_EXCEPTION(hr);
    return SUCCEEDED(hr);
}

void transform_aac_encoder::initialize(bitrate_t bitrate)
{
    HRESULT hr = S_OK;

    IMFActivate** activate = NULL;
    UINT count = 0;
    MFT_REGISTER_TYPE_INFO info = {MFMediaType_Audio, MFAudioFormat_AAC};
    const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;

    CHECK_HR(hr = MFTEnumEx(
        MFT_CATEGORY_AUDIO_ENCODER,
        flags,
        NULL,
        &info,
        &activate,
        &count));

    if(!count)
        CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);

    CHECK_HR(hr = activate[0]->ActivateObject(__uuidof(IMFTransform), (void**)&this->encoder));

    // set input type
    /*this->input_type = input_type;*/
    CHECK_HR(hr = MFCreateMediaType(&this->input_type));
    /*CHECK_HR(hr = input_type->CopyAllItems(this->input_type));*/
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, sizeof(bit_depth_t) * 8));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));

    // set output type
    CHECK_HR(hr = MFCreateMediaType(&this->output_type));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, sizeof(bit_depth_t) * 8));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate));

    // get streams
    DWORD input_stream_count, output_stream_count;
    CHECK_HR(hr = this->encoder->GetStreamCount(&input_stream_count, &output_stream_count));
    if(input_stream_count != 1 || output_stream_count != 1)
        CHECK_HR(hr = MF_E_TOPO_CODEC_NOT_FOUND);
    hr = this->encoder->GetStreamIDs(
        input_stream_count, &this->input_id, output_stream_count, &this->output_id);
    if(hr == E_NOTIMPL)
        this->input_id = this->output_id = 0;
    else if(FAILED(hr))
        CHECK_HR(hr);

    // set stream types
    CHECK_HR(hr = this->encoder->SetInputType(this->input_id, this->input_type, 0));
    CHECK_HR(hr = this->encoder->SetOutputType(this->output_id, this->output_type, 0));

    // get stream info
    CHECK_HR(hr = this->encoder->GetInputStreamInfo(this->input_id, &this->input_stream_info));
    CHECK_HR(hr = this->encoder->GetOutputStreamInfo(this->output_id, &this->output_stream_info));

    CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));

done:
    if(activate)
    {
        for(UINT i = 0; i < count; i++)
            activate[i]->Release();
        CoTaskMemFree(activate);
    }

    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

media_stream_t transform_aac_encoder::create_stream(presentation_clock_t&& clock)
{
    media_stream_clock_sink_t stream(
        new stream_aac_encoder(this->shared_from_this<transform_aac_encoder>()));
    stream->register_sink(clock);

    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_aac_encoder::stream_aac_encoder(const transform_aac_encoder_t& transform) : 
    media_stream_clock_sink(transform.get()),
    transform(transform),
    drain_point(std::numeric_limits<time_unit>::min())
{
}

void stream_aac_encoder::on_component_start(time_unit t)
{
    if(this->transform->time_shift < 0)
        this->transform->time_shift = t;
}

void stream_aac_encoder::on_component_stop(time_unit t)
{
    this->drain_point = t;
}

media_stream::result_t stream_aac_encoder::request_sample(request_packet& rp, const media_stream*)
{
    return this->transform->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_aac_encoder::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream*)
{
    const media_sample_audio& audio_sample = reinterpret_cast<const media_sample_audio&>(sample_view);

    transform_aac_encoder::request_t request;
    request.rp = rp;
    request.sample_view.audio = audio_sample;
    request.sample_view.drain = (this->drain_point == rp.request_time);
    request.stream = this;

    this->transform->requests.push(request);

    this->transform->processing_cb(NULL);
    return OK;
}