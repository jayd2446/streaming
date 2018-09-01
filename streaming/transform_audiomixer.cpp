#include "transform_audiomixer.h"
#include "transform_audioprocessor.h"
#include "assert.h"
#include <Mferror.h>
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#undef min
#undef max

transform_audiomixer::transform_audiomixer(const media_session_t& session) : 
    media_source(session),
    next_position3(0),
    out_next_position(0),
    leftover_audio(media_buffer_samples_t(new media_buffer_samples)),
    next_position_initialized(false)
{
}

void transform_audiomixer::initialize()
{
    // TODO: left over audio must be dismissed if the input data has been changed
    this->leftover_audio.bit_depth = sizeof(transform_audioprocessor::bit_depth_t) * 8;
    this->leftover_audio.channels = transform_aac_encoder::channels;
    this->leftover_audio.sample_rate = transform_aac_encoder::sample_rate;
}

stream_audiomixer_t transform_audiomixer::create_stream(presentation_clock_t& clock)
{
    stream_audiomixer_t stream(new stream_audiomixer(this->shared_from_this<transform_audiomixer>()));
    stream->register_sink(clock);

    return stream;
}

void transform_audiomixer::try_initialize_next_positions(time_unit request_time)
{
    if(!this->next_position_initialized)
    {
        this->next_position_initialized = true;

        const double frame_duration = SECOND_IN_TIME_UNIT / (double)transform_aac_encoder::sample_rate;
        this->out_next_position = this->next_position3 = (frame_unit)(request_time / frame_duration);
    }
}

bool transform_audiomixer::mix(media_buffer_samples& out, const media_sample_audio& in)
{
    // discard in frames that are at earlier point than next_position;
    // cache the leftover buffer that didn't fit into out

    // TODO: buffer locking in case of failure

    assert_(out.samples.size() == 1);
    assert_(in.bit_depth == sizeof(transform_audioprocessor::bit_depth_t) * 8);
    assert_(in.channels == transform_aac_encoder::channels);
    assert_(in.sample_rate == transform_aac_encoder::sample_rate);

    typedef transform_audioprocessor::bit_depth_t bit_depth_in_t;

    HRESULT hr = S_OK;
    LONGLONG pos, dur;
    frame_unit frame_start, frame_end;
    CComPtr<IMFMediaBuffer> out_buffer;
    bit_depth_t* out_data_base = NULL;
    bool fully_mixed = true;

    CHECK_HR(hr = out.samples.front()->GetSampleTime(&pos));
    CHECK_HR(hr = out.samples.front()->GetSampleDuration(&dur));
    frame_start = pos;
    frame_end = pos + dur;

    CHECK_HR(hr = out.samples.front()->GetBufferByIndex(0, &out_buffer));
    CHECK_HR(hr = out_buffer->Lock((BYTE**)&out_data_base, NULL, NULL));
    
    for(auto it = in.buffer->samples.begin(); it != in.buffer->samples.end(); it++)
    {
        LONGLONG pos, dur;
        frame_unit frame_start_in, frame_end_in;
        frame_unit start_cut, end_cut;
        frame_unit sample_start, sample_end;
        CComPtr<IMFMediaBuffer> in_buffer;
        bit_depth_in_t* in_data = NULL;
        bit_depth_t* out_data = NULL;

        CHECK_HR(hr = (*it)->GetSampleTime(&pos));
        CHECK_HR(hr = (*it)->GetSampleDuration(&dur));

        frame_start_in = pos;
        frame_end_in = pos + dur;

        // discard frames that are too old
        if(frame_end_in <= frame_start)
        {
            std::cout << "discarded" << std::endl;
            continue;
        }

        start_cut = std::max(0LL, frame_start - frame_start_in);
        end_cut = std::max(0LL, std::min(frame_end_in - frame_start_in, frame_end_in - frame_end));

        CHECK_HR(hr = (*it)->GetBufferByIndex(0, &in_buffer));
        CHECK_HR(hr = in_buffer->Lock((BYTE**)&in_data, NULL, NULL));

        in_data += start_cut * in.channels;
        out_data = out_data_base + ((frame_start_in + start_cut) - frame_start) * in.channels;

        sample_start = (frame_start_in + start_cut) * (frame_unit)in.channels;
        sample_end = (frame_end_in - end_cut) * (frame_unit)in.channels;
        for(frame_unit i = sample_start; i < sample_end; i++)
        {
            int64_t temp = *out_data;
            temp += *in_data++;
            /*temp *= std::numeric_limits<bit_depth_t>::max();*/

            // clamp
            *out_data++ = (bit_depth_t)std::max((int64_t)std::numeric_limits<bit_depth_t>::min(),
                std::min((int64_t)temp, (int64_t)std::numeric_limits<bit_depth_t>::max()));
        }

        CHECK_HR(hr = in_buffer->Unlock());

        // add to the leftover buffer if needed
        if(end_cut)
        {
            // the leftover audio is assumed to be mixed to out on every request call;
            // this assume indicates a bug
            assert_(&in != &this->leftover_audio);

            fully_mixed = false;

            // TODO: left over buffer could be implemented the same way audioprocessor
            // handles static buffers
            CComPtr<IMFSample> sample;
            CComPtr<IMFMediaBuffer> buffer;
            const UINT32 buffer_len = (UINT32)end_cut * in.get_block_align();
            bit_depth_in_t* data = NULL;

            CHECK_HR(hr = MFCreateSample(&sample));
            CHECK_HR(hr = sample->SetSampleTime(frame_end_in - end_cut));
            CHECK_HR(hr = sample->SetSampleDuration(end_cut));
            CHECK_HR(hr = MFCreateAlignedMemoryBuffer(buffer_len,
                in.get_block_align() / in.channels - 1, &buffer));
            CHECK_HR(hr = buffer->SetCurrentLength(buffer_len));
            CHECK_HR(hr = sample->AddBuffer(buffer));
            CHECK_HR(hr = buffer->Lock((BYTE**)&data, NULL, NULL));

            for(UINT32 i = 0; i < buffer_len / sizeof(bit_depth_in_t); i++)
                *data++ = *in_data++;

            CHECK_HR(hr = buffer->Unlock());

            this->leftover_audio.buffer->samples.push_back(sample);

           /* CComPtr<IMFSample> sample;
            CComPtr<IMFMediaBuffer> buffer;
            const DWORD wrapper_offset = 
                (DWORD)((frame_end_in - end_cut) - frame_start_in) * in.get_block_align();
            const DWORD wrapper_len = (DWORD)end_cut * in.get_block_align();

            CHECK_HR(hr = MFCreateSample(&sample));
            CHECK_HR(hr = sample->SetSampleTime(frame_end_in - end_cut));
            CHECK_HR(hr = sample->SetSampleDuration(end_cut));
            CHECK_HR(hr = MFCreateMediaBufferWrapper(in_buffer, wrapper_offset, wrapper_len, &buffer));
            CHECK_HR(hr = buffer->SetCurrentLength(wrapper_len));
            CHECK_HR(hr = sample->AddBuffer(buffer));

            this->leftover_audio.buffer->samples.push_back(sample);*/

            // update the end position for leftover buffer
            this->next_position3 = std::max(this->next_position3, frame_end_in);
        }
    }

    CHECK_HR(hr = out_buffer->Unlock());

done:
    if(FAILED(hr))
        throw std::exception();

    return fully_mixed;
}

void transform_audiomixer::process(
    media_sample_audio& audio, const media_sample_audios& audios, bool drain, frame_unit drain_point)
{
    HRESULT hr = S_OK;

    CComPtr<IMFSample> out_sample;
    CComPtr<IMFMediaBuffer> out_buffer;

    assert_(audio.buffer);

    audio.bit_depth = sizeof(transform_aac_encoder::bit_depth_t) * 8;
    audio.channels = transform_aac_encoder::channels;
    audio.sample_rate = transform_aac_encoder::sample_rate;
    // audio buffer is assumed to be provided
    /*audio.buffer.reset(new media_buffer_samples);*/

    const frame_unit frames_start = this->out_next_position;
    frame_unit frames_end = this->next_position3;
    bool frames_end_set = false;
    // find the min frame end;
    // on drain max frame end is set
    for(auto it = audios.begin(); it != audios.end(); it++)
    {
        // the frame end is invalid if the buffer is empty and the silent flag
        // isn't set
        if(!it->silent && it->buffer->samples.empty())
            continue;

        if(!frames_end_set)
            frames_end = it->frame_end, frames_end_set = true;
        else
        {
            if(!drain)
                frames_end = std::min(frames_end, it->frame_end);
            else
                frames_end = std::max(frames_end, it->frame_end);
        }
    }
    frames_end = std::max(frames_start, std::max(this->next_position3, frames_end));
    if(drain)
        frames_end = std::max(frames_end, drain_point);
    assert_(frames_end >= frames_start);

     // allocate the out buffer
    const DWORD len = (DWORD)(frames_end - frames_start) * audio.get_block_align();
    if(len)
    {
        bit_depth_t* out_data = NULL;
        CHECK_HR(hr = MFCreateSample(&out_sample));
        CHECK_HR(hr = out_sample->SetSampleTime(frames_start));
        CHECK_HR(hr = out_sample->SetSampleDuration(frames_end - frames_start));
        CHECK_HR(hr = MFCreateAlignedMemoryBuffer(len, 
            audio.get_block_align() / audio.channels - 1, &out_buffer));
        CHECK_HR(hr = out_buffer->SetCurrentLength(len));
        CHECK_HR(hr = out_sample->AddBuffer(out_buffer));
        CHECK_HR(hr = out_buffer->Lock((BYTE**)&out_data, NULL, NULL));
        memset(out_data, 0, len);
        CHECK_HR(hr = out_buffer->Unlock());

        audio.buffer->samples.push_back(out_sample);

        // mix the left over buffer
        const bool fully_mixed = this->mix(*audio.buffer, this->leftover_audio);
        assert_(fully_mixed);
        this->leftover_audio.buffer->samples.clear();

        // mix inputs
        for(auto it = audios.begin(); it != audios.end(); it++)
            this->mix(*audio.buffer, *it);
    }

    // set the out buffer next position
    this->out_next_position = frames_end;

    // set the frame end
    audio.frame_end = frames_end;

    if(drain)
        std::cout << "drain on audiomixer" << std::endl;

done:
    if(FAILED(hr))
        throw std::exception();
}

void transform_audiomixer::process()
{
    std::unique_lock<std::mutex> lock(this->process_mutex);

    request_t request;
    while(this->requests.pop(request))
    {
        const double frame_duration = SECOND_IN_TIME_UNIT / (double)transform_aac_encoder::sample_rate;
        const frame_unit drain_point = (frame_unit)(request.sample_view.drain_point / frame_duration);
        stream_audiomixer* stream = reinterpret_cast<stream_audiomixer*>(request.stream);
        media_sample_audio audio(stream->audio_buffer);

        this->process(audio, *request.sample_view.audios, request.sample_view.drain, drain_point);

        lock.unlock();
        this->session->give_sample(request.stream, audio, request.rp, false);
        lock.lock();
    }
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_audiomixer::stream_audiomixer(const transform_audiomixer_t& transform) :
    transform(transform),
    input_stream_count(0),
    samples_received(0),
    drain_point(std::numeric_limits<time_unit>::min()),
    media_stream_clock_sink(transform.get()),
    audio_buffer(new media_buffer_samples)
{
}

void stream_audiomixer::on_component_start(time_unit t)
{
    this->transform->try_initialize_next_positions(t);
}

void stream_audiomixer::on_component_stop(time_unit t)
{
    this->drain_point = t;
}

void stream_audiomixer::on_stream_start(time_unit)
{
    if(this->input_stream_count == 0)
        throw std::exception();
    this->audios.reserve(this->input_stream_count);
}

void stream_audiomixer::connect_streams(const media_stream_t& from, const media_topology_t& topology)
{
    this->input_stream_count++;
    media_stream::connect_streams(from, topology);
}

media_stream::result_t stream_audiomixer::request_sample(request_packet& rp, const media_stream*)
{
    // make the buffer ready for the next iteration
    this->audios.clear();
    return this->transform->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_audiomixer::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream* prev_stream)
{
    std::unique_lock<std::mutex> lock(this->mutex);

    const media_sample_audio& audio_sample = reinterpret_cast<const media_sample_audio&>(sample_view);

    this->samples_received++;
    this->audios.push_back(audio_sample);

    assert_(this->audios.capacity() >= this->samples_received);
    if(this->samples_received == this->input_stream_count)
    {
        this->samples_received = 0;

        // make request
        transform_audiomixer::request_t request;
        request.rp = rp;
        request.stream = this;
        request.sample_view.drain = (this->drain_point == rp.request_time);
        request.sample_view.drain_point = this->drain_point;
        request.sample_view.audios = &this->audios;

        // clear the output buffer
        this->audio_buffer->samples.clear();

        // push the request
        this->transform->requests.push(request);

        this->transform->process();
    }

    return OK;
}