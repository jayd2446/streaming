#include "transform_aac_encoder.h"
#include <Mferror.h>
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#undef min
#undef max

transform_aac_encoder::transform_aac_encoder(const media_session_t& session) : 
    media_source(session),
    last_time_stamp(std::numeric_limits<frame_unit>::min())
{
}

void transform_aac_encoder::processing_cb(void*)
{
    HRESULT hr = S_OK;

    std::unique_lock<std::recursive_mutex> lock(this->encoder_mutex);
    request_t request;

    while(this->requests.pop(request))
    {
        if(!request.sample_view)
        {
            lock.unlock();
            this->session->give_sample(request.stream, request.sample_view, request.rp, false);
            lock.lock();
        }
        else
        {
            media_buffer_samples_t samples_buffer = 
                request.sample_view->get_buffer<media_buffer_samples>();
            media_buffer_samples_t output_samples_buffer(new media_buffer_samples);
            const double sample_duration = SECOND_IN_TIME_UNIT / (double)samples_buffer->sample_rate;

            for(auto it = samples_buffer->samples.begin(); it != samples_buffer->samples.end(); it++)
            {
                // convert the frame units to time units
                frame_unit ts, dur;
                CHECK_HR(hr = (*it)->GetSampleTime(&ts));
                CHECK_HR(hr = (*it)->GetSampleDuration(&dur));
                ts = (time_unit)(ts * sample_duration);
                dur = (time_unit)(dur * sample_duration);
                CHECK_HR(hr = (*it)->SetSampleTime(ts));
                CHECK_HR(hr = (*it)->SetSampleDuration(dur));
                
#ifdef _DEBUG
                if(ts <= this->last_time_stamp)
                    DebugBreak();
                this->last_time_stamp = ts;
#endif
                /*std::cout << "ts: " << ts << ", dur+ts: " << ts + dur << std::endl;*/

            back:
                hr = this->encoder->ProcessInput(this->input_id, *it, 0);
                if(hr == MF_E_NOTACCEPTING)
                {
                    this->process_output_cb(&request, output_samples_buffer);
                    goto back;
                }
                else
                    CHECK_HR(hr)
            }

            if(output_samples_buffer->samples.empty())
            {
                lock.unlock();
                media_sample_view_t sample_view;
                this->session->give_sample(request.stream, sample_view, request.rp, false);
                lock.lock();
            }
            else
            {
                media_sample_view_t sample_view(new media_sample_view(output_samples_buffer));
                this->session->give_sample(request.stream, sample_view, request.rp, false);
            }
        }
    }

done:
    if(FAILED(hr) && hr != MF_E_NOTACCEPTING)
        throw std::exception();
}

bool transform_aac_encoder::process_output_cb(request_t*, media_buffer_samples_t& out)
{
    HRESULT hr = S_OK;

    const DWORD mft_provides_samples =
        this->output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;

    MFT_OUTPUT_DATA_BUFFER output;
    DWORD status = 0;
    CComPtr<IMFSample> sample;
    CComPtr<IMFMediaBuffer> buffer;

    if(mft_provides_samples)
        throw std::exception();
    if(this->output_stream_info.cbAlignment)
        throw std::exception();

    CHECK_HR(hr = MFCreateSample(&sample));
    CHECK_HR(hr = MFCreateMemoryBuffer(this->output_stream_info.cbSize, &buffer));

    CHECK_HR(hr = sample->AddBuffer(buffer));

    output.dwStreamID = this->output_id;
    output.dwStatus = 0;
    output.pEvents = NULL;
    output.pSample = sample;

    // The size of the pOutputSamples array must be equal to
    // or greater than the number of selected output streams
    CHECK_HR(hr = this->encoder->ProcessOutput(0, 1, &output, &status));

done:
    if(hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        return false;
        /*media_sample_view_t sample_view;
        this->session->give_sample(request->stream, sample_view, request->rp, false);*/
    }
    else if(FAILED(hr))
        throw std::exception();
    else
    {
        static LONGLONG dur = 0;
        /*CHECK_HR(hr = sample->SetSampleTime(dur));*/
        /*CHECK_HR(hr = sample->SetSampleDuration(213334));*/
        out->samples.push_back(sample);

        dur += 213334+1;
        /*dur = 0;*/

        /*media_buffer_aac_t buffer(new media_buffer_aac);
        buffer->sample = sample;
        media_sample_t sample(new media_sample);
        sample->buffer = buffer;
        media_sample_view_t sample_view(new media_sample_view(sample));
        this->session->give_sample(request->stream, sample_view, request->rp, false);*/
    }

    return true;
}

HRESULT transform_aac_encoder::initialize()
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

    const UINT32 samples_per_second = 48000;
    const UINT32 channels = 2;

    // set input type
    /*this->input_type = input_type;*/
    CHECK_HR(hr = MFCreateMediaType(&this->input_type));
    /*CHECK_HR(hr = input_type->CopyAllItems(this->input_type));*/
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->input_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, samples_per_second));
    CHECK_HR(hr = this->input_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));

    // set output type
    CHECK_HR(hr = MFCreateMediaType(&this->output_type));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    CHECK_HR(hr = this->output_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, samples_per_second));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels));
    CHECK_HR(hr = this->output_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (192 * 1000) / 8));

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
        throw std::exception();

    return hr;
}

media_stream_t transform_aac_encoder::create_stream()
{
    return media_stream_t(new stream_aac_encoder(this->shared_from_this<transform_aac_encoder>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_aac_encoder::stream_aac_encoder(const transform_aac_encoder_t& transform) : transform(transform)
{
}

media_stream::result_t stream_aac_encoder::request_sample(request_packet& rp, const media_stream*)
{
    return this->transform->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_aac_encoder::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    transform_aac_encoder::request_t request;
    request.rp = rp;
    request.sample_view = sample_view;
    request.stream = this;

    this->transform->requests.push(request);

    this->transform->processing_cb(NULL);
    return OK;
}