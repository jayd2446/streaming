#include "transform_aac_encoder.h"
#include <Mferror.h>
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
#undef min
#undef max

transform_aac_encoder::transform_aac_encoder(const media_session_t& session) : 
    media_component(session),
    last_time_stamp(std::numeric_limits<frame_unit>::min()),
    time_shift(-1),
    buffer_pool_memory(new buffer_pool_memory_t),
    encoded_audio(new media_sample_aac_frames),
    dispatcher(new request_dispatcher)
{
}

transform_aac_encoder::~transform_aac_encoder()
{
    buffer_pool_memory_t::scoped_lock lock(this->buffer_pool_memory->mutex);
    this->buffer_pool_memory->dispose();
}

bool transform_aac_encoder::encode(const media_sample_audio_frames& in_frames,
    media_sample_aac_frames& out_frames, bool drain)
{
    assert_(out_frames.frames.empty());

    HRESULT hr = S_OK;
    media_buffer_memory_t buffer;
    CComPtr<IMFSample> out_sample;
    CComPtr<IMFMediaBuffer> out_buffer;

    // this is just a specialized case of audio resampler's reset_sample
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
            // TODO: the sink writer should call a place marker method for knowing
            // when the output buffer is safe to reuse;
            // currently, a new buffer must be allocated each time
            buffer_pool_memory_t::scoped_lock lock(this->buffer_pool_memory->mutex);
            buffer.reset(new media_buffer_memory);
            /*buffer = this->buffer_pool_memory->acquire_buffer();*/
            buffer->initialize(this->output_stream_info.cbSize);
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
        media_sample_aac_frame frame;
        LONGLONG ts, dur;

        CHECK_HR(hr = out_sample->GetBufferByIndex(0, &mf_buffer));
        CHECK_HR(hr = mf_buffer->GetCurrentLength(&buflen));

        CHECK_HR(hr = out_sample->GetSampleTime(&ts));
        CHECK_HR(hr = out_sample->GetSampleDuration(&dur));

        frame.memory_host = buffer;
        frame.ts = (time_unit)ts;
        frame.dur = (time_unit)dur;
        frame.buffer = mf_buffer;
        frame.sample = out_sample;

        out_frames.frames.push_back(std::move(frame));

        // by empirical evidence, it seems that the encoder doesn't buffer input samples
        this->memory_hosts.clear();

    done:
        return hr;
    };
    auto drain_all = [&]()
    {
        std::cout << "drain on aac encoder" << std::endl;

        HRESULT hr = S_OK;
        CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
        while(reset_sample(), this->process_output(out_sample))
            CHECK_HR(hr = process_sample());

    done:
        return hr;
    };

    // currently, the audio mixer only outputs one frame
    for(auto&& elem : in_frames.frames)
    {
        assert_(elem.buffer);

        // create a sample that has time and duration converted from frame unit to time unit
        CComPtr<IMFSample> in_sample;
        CComPtr<IMFMediaBuffer> in_buffer;
        frame_unit frame_pos, frame_dur;
        LONGLONG time, dur;

        CHECK_HR(hr = MFCreateSample(&in_sample));
        in_buffer = elem.buffer;
        CHECK_HR(hr = in_sample->AddBuffer(in_buffer));

        frame_pos = elem.pos;
        frame_dur = elem.dur;
        time = (LONGLONG)(convert_to_time_unit(frame_pos, sample_rate, 1) - this->time_shift);
        dur = (LONGLONG)convert_to_time_unit(frame_dur, sample_rate, 1);

        if(time < 0)
        {
            LONGLONG off = time;
            time = 0;
            std::cout << "aac encoder time shift was off by " << off << std::endl;
        }

        CHECK_HR(hr = in_sample->SetSampleTime(time));
        CHECK_HR(hr = in_sample->SetSampleDuration(dur));

    back:
        hr = this->encoder->ProcessInput(this->input_id, in_sample, 0);
        if(hr == MF_E_NOTACCEPTING)
        {
            if(reset_sample(), this->process_output(out_sample))
                CHECK_HR(hr = process_sample());

            goto back;
        }
        else
        {
            CHECK_HR(hr);
            this->memory_hosts.push_back(elem.memory_host);
        }
    }

    if(drain)
        CHECK_HR(hr = drain_all());

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);

    return !out_frames.frames.empty();
}

bool transform_aac_encoder::on_serve(request_queue::request_t& request)
{
    const bool non_null_request = request.sample.drain ||
        (request.sample.args && request.sample.args->has_frames);
    media_component_aac_encoder_args_t& args = request.sample.args;
    media_component_aac_audio_args_t out_args;

    // null requests were passed already
    if(non_null_request)
    {
        assert_(args->is_valid());
        if(this->encode(*args->sample, *request.sample.out_sample, request.sample.drain))
        {
            out_args = std::make_optional<media_component_aac_audio_args>();
            out_args->sample = std::move(request.sample.out_sample);
        }

        request_dispatcher::request_t dispatcher_request;
        dispatcher_request.stream = request.stream;
        dispatcher_request.rp = request.rp;
        dispatcher_request.sample = out_args;

        this->dispatcher->dispatch_request(std::move(dispatcher_request),
            [this_ = this->shared_from_this<transform_aac_encoder>()](
                request_dispatcher::request_t& request)
        {
            this_->session->give_sample(request.stream,
                request.sample.has_value() ? &(*request.sample) : NULL, request.rp);
        });
    }

    return true;
}

transform_aac_encoder::request_queue::request_t* transform_aac_encoder::next_request()
{
    return this->requests.get();
}

bool transform_aac_encoder::process_output(IMFSample* sample)
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

    output.dwStreamID = 0;
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

    CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL));
    CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
    CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

    /*CHECK_HR(hr = this->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));*/

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

media_stream_t transform_aac_encoder::create_stream(media_message_generator_t&& message_generator)
{
    media_stream_message_listener_t stream(
        new stream_aac_encoder(this->shared_from_this<transform_aac_encoder>()));
    stream->register_listener(message_generator);

    return stream;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_aac_encoder::stream_aac_encoder(const transform_aac_encoder_t& transform) : 
    media_stream_message_listener(transform.get()),
    transform(transform),
    buffer_pool_aac_frames(new buffer_pool_aac_frames_t),
    stopping(false)
{
}

stream_aac_encoder::~stream_aac_encoder()
{
    buffer_pool_aac_frames_t::scoped_lock lock(this->buffer_pool_aac_frames->mutex);
    this->buffer_pool_aac_frames->dispose();
}

void stream_aac_encoder::on_component_start(time_unit t)
{
    if(this->transform->time_shift < 0)
        this->transform->time_shift = t;
}

void stream_aac_encoder::on_component_stop(time_unit t)
{
    this->stopping = true;
    /*this->drain_point = t;*/
}

media_stream::result_t stream_aac_encoder::request_sample(const request_packet& rp, const media_stream*)
{
    this->transform->requests.initialize_queue(rp);
    return this->transform->session->request_sample(this, rp) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_aac_encoder::process_sample(
    const media_component_args* args_, const request_packet& rp, const media_stream*)
{
    media_sample_aac_frames_t out_sample;
    transform_aac_encoder::request_t request;
    if(args_)
    {
        const media_component_aac_encoder_args& args = 
            static_cast<const media_component_aac_encoder_args&>(*args_);

        // aac encoder expects full buffers, so the args should include the full buffer
        // (is_valid call tests this)
        assert_(args.is_valid());

        buffer_pool_aac_frames_t::scoped_lock lock(this->buffer_pool_aac_frames->mutex);
        out_sample = this->buffer_pool_aac_frames->acquire_buffer();
        out_sample->initialize();

        request.sample.args = std::make_optional(args);
    }

    request.rp = rp;
    request.sample.drain = (rp.flags & FLAG_LAST_PACKET) && this->stopping;
    request.stream = this;
    request.sample.out_sample = out_sample;

    this->transform->requests.push(request);

    // pass null requests downstream
    if(!request.sample.drain && (!request.sample.args || !request.sample.args->has_frames))
        this->transform->session->give_sample(this, NULL, request.rp);

    this->transform->serve();

    return OK;
}