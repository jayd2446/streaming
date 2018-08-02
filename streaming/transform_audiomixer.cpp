#include "transform_audiomixer.h"
#include "assert.h"
#include <Mferror.h>
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}
#undef min
#undef max

transform_audiomixer::transform_audiomixer(const media_session_t& session) : 
    media_source(session),
    /*next_position(0),
    next_position2(0),*/
    next_position3(0),
    out_next_position(0),
    leftover_audio(media_buffer_samples_t(new media_buffer_samples)),
    next_position_initialized(false)
{
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
        this->out_next_position = /*this->next_position =*/ 
            /*this->next_position2 =*/ this->next_position3 = (frame_unit)(request_time / frame_duration);
    }
}

bool transform_audiomixer::mix(
    media_buffer_samples& out, const media_sample_audio& in, frame_unit& next_position)
{
    // discard in frames that are at earlier point than next_position;
    // cache the leftover buffer that didn't fit into out

    // TODO: buffer locking in case of failure

    assert_(out.samples.size() == 1);

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
        /*assert_(next_position >= frame_start);*/

        LONGLONG pos, dur;
        frame_unit frame_start_in, frame_end_in;
        frame_unit start_cut, end_cut;
        frame_unit sample_start, sample_end;
        CComPtr<IMFMediaBuffer> in_buffer;
        bit_depth_t* in_data = NULL, *out_data = NULL;

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

            // clamp
            if(temp > std::numeric_limits<bit_depth_t>::max())
                temp = std::numeric_limits<bit_depth_t>::max();
            else if(temp < std::numeric_limits<bit_depth_t>::min())
                temp = std::numeric_limits<bit_depth_t>::min();

            *out_data++ = (bit_depth_t)temp;
        }

        // TODO: leftover buffer can be a wrapper
        // add to the leftover buffer if needed
        if(end_cut)
        {
            // TODO: the leftover buffer is greater than the minimum length of the new buffers
            // (should be fine)

            // (the left over buffer will make a copy of itself, and the second copy will be discarded
            // when the audio is mixed to output buffer);
            // this happens when the audio samples are discarded at clock start
            assert_(&in != &this->leftover_audio);
            //if(&in == &this->leftover_audio)
            //{
            //    std::cout << "short buffers" << std::endl;
            //    /*goto out;*/
            //}

            fully_mixed = false;

            CComPtr<IMFSample> sample;
            CComPtr<IMFMediaBuffer> buffer;
            const UINT32 buffer_len = (UINT32)end_cut * in.get_block_align();
            bit_depth_t* data = NULL;

            CHECK_HR(hr = MFCreateSample(&sample));
            CHECK_HR(hr = sample->SetSampleTime(frame_end_in - end_cut));
            CHECK_HR(hr = sample->SetSampleDuration(end_cut));
            CHECK_HR(hr = MFCreateAlignedMemoryBuffer(buffer_len, 
                in.get_block_align() / in.channels - 1, &buffer));
            CHECK_HR(hr = buffer->SetCurrentLength(buffer_len));
            CHECK_HR(hr = sample->AddBuffer(buffer));
            CHECK_HR(hr = buffer->Lock((BYTE**)&data, NULL, NULL));

            for(UINT32 i = 0; i < buffer_len / sizeof(bit_depth_t); i++)
                *data++ = *in_data++;

            CHECK_HR(hr = buffer->Unlock());

            this->leftover_audio.buffer->samples.push_back(sample);
        }
    /*out:*/

        CHECK_HR(hr = in_buffer->Unlock());

        // update the next position
        next_position = std::max(frame_end_in, next_position);
    }

    CHECK_HR(hr = out_buffer->Unlock());

done:
    if(FAILED(hr))
        throw std::exception();

    return fully_mixed;
}

/*
TODO: master audio mixer should be tied to encoder
TODO: master audio mixer should allow single streams
TODO: on clock stop call, add audio processor drain resampler flag
*/

void transform_audiomixer::process(
    media_sample_audio& audio, const media_sample_audios& audios, bool drain)
{
    HRESULT hr = S_OK;

    CComPtr<IMFSample> out_sample;
    CComPtr<IMFMediaBuffer> out_buffer;
    bit_depth_t* out_data = NULL;
    media_sample_audio leftover_audio;

    audio.bit_depth = sizeof(transform_aac_encoder::bit_depth_t) * 8;
    audio.channels = transform_aac_encoder::channels;
    audio.sample_rate = transform_aac_encoder::sample_rate;
    audio.buffer.reset(new media_buffer_samples);

    // TODO: left over audio must be dismissed if the input data has been changed
    this->leftover_audio.bit_depth = sizeof(transform_aac_encoder::bit_depth_t) * 8;
    this->leftover_audio.channels = transform_aac_encoder::channels;
    this->leftover_audio.sample_rate = transform_aac_encoder::sample_rate;

    const frame_unit frames_start = this->out_next_position;
    frame_unit frames_end = this->next_position3;
    bool frames_end_set = false;
    /*assert_(frames_end >= frames_start);*/
    // find the min frame end;
    // on drain max frame end is set
    for(auto it = audios.begin(); it != audios.end(); it++)
    {
        if(!drain)
            if(!frames_end_set)
                frames_end = it->frame_end, frames_end_set = true;
            else
                frames_end = std::min(frames_end, it->frame_end);
        else
            if(!frames_end_set)
                frames_end = it->frame_end, frames_end_set = true;
            else
                frames_end = std::max(frames_end, it->frame_end);
    }
    frames_end = std::max(frames_start, std::max(this->next_position3, frames_end));
    assert_(frames_end >= frames_start);

     // allocate the out buffer
    const DWORD len = (DWORD)(frames_end - frames_start) * audio.get_block_align();
    if(len)
    {
        CHECK_HR(hr = MFCreateSample(&out_sample));
        CHECK_HR(hr = out_sample->SetSampleTime(frames_start));
        CHECK_HR(hr = out_sample->SetSampleDuration(frames_end - frames_start));
        CHECK_HR(hr = MFCreateAlignedMemoryBuffer(len, 
            audio.get_block_align() / audio.channels - 1, &out_buffer));
        CHECK_HR(hr = out_buffer->SetCurrentLength(len));
        CHECK_HR(hr = out_sample->AddBuffer(out_buffer));
        CHECK_HR(hr = out_buffer->Lock((BYTE**)&out_data, NULL, NULL));
        for(UINT32 i = 0; i < len / sizeof(bit_depth_t); i++)
            *out_data++ = 0;
        CHECK_HR(hr = out_buffer->Unlock());

        audio.buffer->samples.push_back(out_sample);

        // mix the left over buffer
        const bool fully_mixed = this->mix(*audio.buffer, this->leftover_audio, this->next_position3);
        assert_(fully_mixed);
        this->leftover_audio.buffer->samples.clear();

        // mix inputs
        for(auto it = audios.begin(); it != audios.end(); it++)
        {
            frame_unit next_position = this->out_next_position;
            this->mix(*audio.buffer, *it, next_position);
        }
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

//void transform_audiomixer::process(
//    media_sample_audio& audio, 
//    const media_sample_audio& audio_sample, 
//    const media_sample_audio& audio_sample2, 
//    int input_stream_count,
//    bool drain)
//{
//    HRESULT hr = S_OK;
//
//    CComPtr<IMFSample> out_sample;
//    CComPtr<IMFMediaBuffer> out_buffer;
//    bit_depth_t* out_data = NULL;
//    media_sample_audio leftover_audio;
//
//    /*assert_(audio_sample.bit_depth == (sizeof(transform_aac_encoder::bit_depth_t) * 8) && 
//        audio_sample2.bit_depth == (sizeof(transform_aac_encoder::bit_depth_t) * 8));
//    assert_(audio_sample.channels == audio_sample2.channels);
//    assert_(audio_sample.sample_rate == audio_sample2.sample_rate);*/
//
//    audio.bit_depth = sizeof(transform_aac_encoder::bit_depth_t) * 8;
//    audio.channels = transform_aac_encoder::channels;
//    audio.sample_rate = transform_aac_encoder::sample_rate;
//    audio.buffer.reset(new media_buffer_samples);
//
//    // TODO: left over audio must be dismissed if the input data has been changed
//    this->leftover_audio.bit_depth = sizeof(transform_aac_encoder::bit_depth_t) * 8;
//    this->leftover_audio.channels = transform_aac_encoder::channels;
//    this->leftover_audio.sample_rate = transform_aac_encoder::sample_rate;
//
//    // calculate frame start position and frame end position
//    // (the out buffer length is at least the size of the leftover buffer)
//    const frame_unit frames_start = this->out_next_position;//std::min(this->next_position, this->next_position2);
//    LONGLONG request_end = frames_start, request_end2 = frames_start;
//    if(!audio_sample.buffer->samples.empty())
//        request_end = audio_sample.frame_end;
//    if(!audio_sample2.buffer->samples.empty())
//        request_end2 = audio_sample2.frame_end;
//
//    frame_unit frames_end;
//    if(input_stream_count == 2)
//        if(!drain)
//            frames_end = std::max(frames_start, std::min(request_end, request_end2));
//        else
//            // on drain make sure that there won't be a leftover buffer
//            frames_end =
//            std::max(frames_start, std::max(std::max(request_end, request_end2), this->next_position3));
//    else if(input_stream_count == 1)
//        frames_end = std::max(frames_start, request_end);
//    else
//        throw std::exception();
//
//    /*const frame_unit frames_end = 
//        std::max(frames_start, std::min(request_end, request_end2));*/
//
//    // allocate the out buffer
//    const DWORD len = (DWORD)(frames_end - frames_start) * audio.get_block_align();
//    CHECK_HR(hr = MFCreateSample(&out_sample));
//    CHECK_HR(hr = out_sample->SetSampleTime(frames_start));
//    CHECK_HR(hr = out_sample->SetSampleDuration(frames_end - frames_start));
//    CHECK_HR(hr = MFCreateAlignedMemoryBuffer(len, 
//        audio.get_block_align() / audio.channels - 1, &out_buffer));
//    CHECK_HR(hr = out_buffer->SetCurrentLength(len));
//    CHECK_HR(hr = out_sample->AddBuffer(out_buffer));
//    CHECK_HR(hr = out_buffer->Lock((BYTE**)&out_data, NULL, NULL));
//    for(UINT32 i = 0; i < len / sizeof(bit_depth_t); i++)
//        *out_data++ = 0;
//    CHECK_HR(hr = out_buffer->Unlock());
//    audio.buffer->samples.push_back(out_sample);
//
//    // set the out buffer next position
//    this->out_next_position = frames_end;
//
//    // mix the left over buffer
//    leftover_audio = this->leftover_audio;
//    this->leftover_audio.buffer.reset(new media_buffer_samples);
//    const bool fully_mixed = this->mix(*audio.buffer, leftover_audio, this->next_position3);
//    assert_(!drain || fully_mixed);
//
//    // mix inputs and set next positions
//    const bool fully_mixed2 = this->mix(*audio.buffer, audio_sample, this->next_position);
//    if(input_stream_count == 2)
//    {
//        const bool fully_mixed3 = this->mix(*audio.buffer, audio_sample2, this->next_position2);
//        assert_(fully_mixed2 || fully_mixed3);
//    }
//    else
//    {
//        this->next_position2 = this->next_position;
//        assert_(fully_mixed2);
//    }
//
//    // set the frame end
//    audio.frame_end = frames_end;
//
//done:
//    if(FAILED(hr))
//        throw std::exception();
//}

void transform_audiomixer::process()
{
    scoped_lock lock(this->process_mutex);

    request_t request;
    while(this->requests.pop(request))
    {
        media_sample_audio audio;
        this->process(audio, request.sample_view.audios, request.sample_view.drain);
        this->session->give_sample(request.stream, audio, request.rp, false);
    }
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_audiomixer::stream_audiomixer(const transform_audiomixer_t& transform) :
    transform(transform),
    /*primary_stream(NULL),*/
    input_stream_count(0),
    samples_received(0),
    drain_point(std::numeric_limits<time_unit>::min()),
    media_stream_clock_sink(transform.get())
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

void stream_audiomixer::connect_streams(const media_stream_t& from, const media_topology_t& topology)
{
    this->input_stream_count++;
    media_stream::connect_streams(from, topology);
}

media_stream::result_t stream_audiomixer::request_sample(request_packet& rp, const media_stream*)
{
    if(this->input_stream_count == 0)
        throw std::exception();

    return this->transform->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_audiomixer::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream* prev_stream)
{
    std::unique_lock<std::mutex> lock(this->mutex);

    const media_sample_audio& audio_sample = reinterpret_cast<const media_sample_audio&>(sample_view);

    this->samples_received++;
    this->audios.push_back(audio_sample);

    if(this->samples_received == this->input_stream_count)
    {
        this->samples_received = 0;

        // set parameters
        this->request.rp = rp;
        this->request.stream = this;
        this->request.sample_view.drain = (this->drain_point == rp.request_time);

        // push the request and transfer list
        transform_audiomixer::media_sample_audios& audios = 
            this->transform->requests.push(this->request).sample_view.audios;
        audios.splice(audios.end(), this->audios);

        this->request = transform_audiomixer::request_t();
        lock.unlock();
        this->transform->process();

        /*media_sample_audio audio;
        this->transform->process(
            audio, this->pending_request, this->pending_request2, this->input_stream_count);

        this->request = transform_audiomixer::request_t();

        lock.unlock();
        
        return this->transform->session->give_sample(this, audio, rp, false) ? OK : FATAL_ERROR;*/
    }

    return OK;
}