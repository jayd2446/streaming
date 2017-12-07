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
            /*std::cout << "processing.." << std::endl;*/
            media_buffer_samples_t samples_buffer = 
                request.sample_view->get_buffer<media_buffer_samples>();
            media_buffer_samples_t output_samples_buffer(new media_buffer_samples);
            const double sample_duration = SECOND_IN_TIME_UNIT / (double)samples_buffer->sample_rate;

            assert_(samples_buffer->bit_depth == (sizeof(bit_depth_t) * 8) &&
                samples_buffer->channels == channels &&
                samples_buffer->sample_rate == sample_rate);

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
                if(FAILED(hr))
                    throw std::exception();
            };

            reset_sample();
            for(auto it = samples_buffer->samples.begin(); it != samples_buffer->samples.end(); it++)
            {
                // convert the frame units to time units
                frame_unit ts, dur;
                CHECK_HR(hr = (*it)->GetSampleTime(&ts));
                CHECK_HR(hr = (*it)->GetSampleDuration(&dur));

#ifdef _DEBUG
                // TODO: this might trigger if the ending request times differ for the mixer streams
                if(ts < this->last_time_stamp)
                    DebugBreak();
                this->last_time_stamp = ts + dur;
#endif

                ts = (time_unit)(ts * sample_duration);
                dur = (time_unit)(dur * sample_duration);
                CHECK_HR(hr = (*it)->SetSampleTime(ts));
                CHECK_HR(hr = (*it)->SetSampleDuration(dur));
               

                /*std::cout << "ts: " << ts << ", dur+ts: " << ts + dur << std::endl;*/

            back:
                hr = this->encoder->ProcessInput(this->input_id, *it, 0);
                if(hr == MF_E_NOTACCEPTING)
                {
                    if(!this->process_output(out_sample))
                        goto back;
                    
                    output_samples_buffer->samples.push_back(out_sample);

                    // reset the out sample
                    out_sample = NULL;
                    out_buffer = NULL;
                    reset_sample();
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

bool transform_aac_encoder::process_output(IMFSample* sample)
{
    HRESULT hr = S_OK;

    const DWORD mft_provides_samples =
        this->output_stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;

    MFT_OUTPUT_DATA_BUFFER output;
    DWORD status = 0;

    if(mft_provides_samples)
        throw std::exception();
    if(this->output_stream_info.cbAlignment)
        throw std::exception();

    output.dwStreamID = this->output_id;
    output.dwStatus = 0;
    output.pEvents = NULL;
    output.pSample = sample;

    CHECK_HR(hr = this->encoder->ProcessOutput(0, 1, &output, &status));

done:
    if(hr != MF_E_TRANSFORM_NEED_MORE_INPUT && FAILED(hr))
        throw std::exception();
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
        throw std::exception();
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