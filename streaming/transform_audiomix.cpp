#include "transform_audiomix.h"
#include "assert.h"
#include <Mferror.h>
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
#undef min
#undef max

transform_audiomix::transform_audiomix(const media_session_t& session) : media_source(session)
{
}

stream_audiomix_t transform_audiomix::create_stream()
{
    return stream_audiomix_t(new stream_audiomix(this->shared_from_this<transform_audiomix>()));
}

CComPtr<IMFMediaBuffer> transform_audiomix::copy(
    UINT32 bit_depth, UINT32 channels,
    const CComPtr<IMFSample>& sample, frame_unit start, frame_unit end) const
{
    HRESULT hr = S_OK;
    CComPtr<IMFMediaBuffer> buffer, out_buffer;
    DWORD buflen;
    const UINT32 block_align = (bit_depth * channels) / 8;
    frame_unit ts;

    CHECK_HR(hr = sample->GetSampleTime(&ts));
    CHECK_HR(hr = sample->GetBufferByIndex(0, &buffer));
    CHECK_HR(hr = buffer->GetCurrentLength(&buflen));
    const DWORD offset_start = (DWORD)((start - ts) * block_align);
    const DWORD offset_end = (DWORD)((end - ts) * block_align);

    assert_(start >= ts);
    assert_(((int)buflen - (int)offset_end) >= 0);
    assert_((int)offset_start < (int)offset_end);

    CHECK_HR(hr = MFCreateMediaBufferWrapper(buffer, offset_start, offset_end - offset_start, &out_buffer));
    CHECK_HR(hr = out_buffer->SetCurrentLength(offset_end - offset_start));

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);

    return out_buffer;
}

CComPtr<IMFMediaBuffer> transform_audiomix::mix(
    UINT32 bit_depth, UINT32 channels,
    const CComPtr<IMFSample>& sample,
    const CComPtr<IMFSample>& sample2,
    frame_unit start, frame_unit end) const
{
    HRESULT hr = S_OK;
    CComPtr<IMFMediaBuffer> buffer, buffer2, out_buffer;
    DWORD buflen, buflen2;
    const UINT32 block_align = (bit_depth * channels) / 8;
    frame_unit ts, ts2;
    typedef int16_t bit_depth_t;
    typedef uint16_t ubit_depth_t;

    CHECK_HR(hr = sample->GetSampleTime(&ts));
    CHECK_HR(hr = sample2->GetSampleTime(&ts2));
    CHECK_HR(hr = sample->GetBufferByIndex(0, &buffer));
    CHECK_HR(hr = sample2->GetBufferByIndex(0, &buffer2));
    CHECK_HR(hr = buffer->GetCurrentLength(&buflen));
    CHECK_HR(hr = buffer2->GetCurrentLength(&buflen2));
    const DWORD offset_start = (DWORD)((start - ts) * block_align);
    const DWORD offset_end = (DWORD)((end - ts) * block_align);
    const DWORD offset_start2 = (DWORD)((start - ts2) * block_align);
    const DWORD offset_end2 = (DWORD)((end - ts2) * block_align);

    assert_(start >= ts);
    assert_(start >= ts2);
    assert_(((int)buflen - (int)offset_end) >= 0);
    assert_(((int)buflen2 - (int)offset_end2) >= 0);
    assert_((int)offset_start < (int)offset_end);
    assert_((int)offset_start2 < (int)offset_end2);
    assert_(sizeof(bit_depth_t) == (bit_depth / 8));
    assert_((int)offset_end - (int)offset_start == (int)offset_end2 - (int)offset_start2);
    assert_(sizeof(bit_depth_t) < sizeof(int64_t));

    CHECK_HR(hr = MFCreateMediaBufferWrapper(buffer, offset_start, offset_end - offset_start, &out_buffer));
    CHECK_HR(hr = out_buffer->SetCurrentLength(offset_end - offset_start));

    // mix
    BYTE* data_out, *data_in;
    CHECK_HR(hr = out_buffer->Lock(&data_out, NULL, NULL));
    CHECK_HR(hr = buffer2->Lock(&data_in, NULL, NULL));
    data_in += offset_start2;
    for(UINT32 i = 0; i < (offset_end - offset_start) / block_align * channels; i++)
    {
        double in = ((bit_depth_t*)data_out)[i], in2 = ((bit_depth_t*)data_in)[i];
        in += in2;
        /*in += std::numeric_limits<bit_depth_t>::max();
        in2 += std::numeric_limits<bit_depth_t>::max();

        in += in2;
        in /= std::numeric_limits<ubit_depth_t>::max() * 2;
        in *= 2;
        in -= 1;
        in *= std::numeric_limits<bit_depth_t>::max();*/

        int32_t out = (int32_t)in;
        // clamp
        if(out > std::numeric_limits<bit_depth_t>::max())
            out = std::numeric_limits<bit_depth_t>::max();
        else if(out < std::numeric_limits<bit_depth_t>::min())
            out = std::numeric_limits<bit_depth_t>::min();

        ((bit_depth_t*)data_out)[i] = (bit_depth_t)out;
    }
    CHECK_HR(hr = buffer2->Unlock());
    CHECK_HR(hr = out_buffer->Unlock());

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);

    return out_buffer;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_audiomix::stream_audiomix(const transform_audiomix_t& transform) : 
    transform(transform),
    primary_stream(NULL)
{
    this->pending_request.rp.packet_number = 
        this->pending_request2.rp.packet_number = INVALID_PACKET_NUMBER;
}

void stream_audiomix::process_cb(void*)
{
    HRESULT hr = S_OK;
    media_sample_audio audio(media_buffer_samples_t(new media_buffer_samples));
    /*media_buffer_samples_t samples(new media_buffer_samples);
    media_sample_view_t sample_view;*/
    request_packet rp = this->pending_request.rp;
    typedef int16_t bit_depth_t;

    media_sample_audio& audio_sample = this->pending_request.audio_sample;
    media_sample_audio& audio_sample2 = this->pending_request2.audio_sample;

    auto it = audio_sample.buffer->samples.begin(), jt = audio_sample2.buffer->samples.begin();
    bool ptr_it = true;
    frame_unit ptr_tp_start = std::numeric_limits<frame_unit>::min();

    assert_(audio_sample.bit_depth == (sizeof(bit_depth_t) * 8) && 
        audio_sample2.bit_depth == (sizeof(bit_depth_t) * 8));
    assert_(audio_sample.channels == audio_sample2.channels);
    assert_(audio_sample.sample_rate == audio_sample2.sample_rate);

    audio.bit_depth = audio_sample.bit_depth;
    audio.channels = audio_sample.channels;
    audio.sample_rate = audio_sample.sample_rate;

    // increment the iterator of the one that has earlier ending timepoint
    // set the pointer to the ending timepoint on the other iterator

    // lock the ptr using ptr_it and ptr_tp info
    // copy the region of ptr_tp and max of the end of ptr and the incremented iterator
    // mix to the earlier ending timepoint
    // set ptr_tp to the earlier timepoint and set ptr_it to the iterator that doesn't have the earlier timepoitn
    // unlock ptr
    // increment the iterator that includes the earlier timepoint
    for(;it != audio_sample.buffer->samples.end() && jt != audio_sample2.buffer->samples.end();
        ptr_it ? jt++ : it++)
    {
        CComPtr<IMFSample> ptr_sample, sample;
        frame_unit ts, ts2, dur, dur2;
        frame_unit tp_start,  tp_end, tp_diff;
        frame_unit ptr_tp_end;

        CHECK_HR(hr = (*it)->GetSampleTime(&ts));
        CHECK_HR(hr = (*jt)->GetSampleTime(&ts2));
        CHECK_HR(hr = (*it)->GetSampleDuration(&dur));
        CHECK_HR(hr = (*jt)->GetSampleDuration(&dur2));

        ptr_it = (ts <= ts2);
        ptr_tp_start = std::max(ptr_tp_start, std::min(ts, ts2));

        tp_start = ptr_it ? ts2 : ts;
        tp_end = ptr_it ? (ts2 + dur2) : (ts + dur);
        ptr_tp_end = ptr_it ? (ts + dur) : (ts2 + dur2);

        // copy part up to ptr end or tp_start
        // mix part up to ptr end or tp_end
        // on end, iterator is incremented and ptr unlocked if ptr end reached

        ptr_sample = ptr_it ? *it : *jt;
        sample = ptr_it ? *jt : *it;

        // copy up to ptr_tp_end, increment it and unlock the ptr
        tp_diff = std::min(ptr_tp_end, tp_start);
        /*assert_((tp_diff - ptr_tp_start) >= 0);*/
        if((tp_diff - ptr_tp_start) > 0)
        {
            CComPtr<IMFSample> new_sample;
            CComPtr<IMFMediaBuffer> buffer;
            CHECK_HR(hr = MFCreateSample(&new_sample));
            buffer = this->transform->copy(
                audio_sample.bit_depth, audio_sample.channels,
                ptr_sample, ptr_tp_start, tp_diff);
            CHECK_HR(hr = new_sample->AddBuffer(buffer));
            CHECK_HR(hr = new_sample->SetSampleTime(ptr_tp_start));
            CHECK_HR(hr = new_sample->SetSampleDuration(tp_diff - ptr_tp_start));
            audio.buffer->samples.push_back(new_sample);

            ptr_tp_start = tp_diff;
            if(ptr_tp_end <= tp_start)
            {
                ptr_it = !ptr_it;
                continue;
            }
        }

        // mix up to tp_diff
        tp_diff = std::min(ptr_tp_end, tp_end);
        {
            CComPtr<IMFSample> new_sample;
            CComPtr<IMFMediaBuffer> buffer;
            CHECK_HR(hr = MFCreateSample(&new_sample));
            buffer = this->transform->mix(
                audio_sample.bit_depth, audio_sample.channels,
                ptr_sample, sample, ptr_tp_start, tp_diff);
            CHECK_HR(hr = new_sample->AddBuffer(buffer));
            CHECK_HR(hr = new_sample->SetSampleTime(ptr_tp_start));
            CHECK_HR(hr = new_sample->SetSampleDuration(tp_diff - ptr_tp_start));
            audio.buffer->samples.push_back(new_sample);
        }
        ptr_tp_start = tp_diff;
        if(ptr_tp_end <= tp_end)
        {
            if(ptr_tp_end == tp_end)
                ptr_it ? jt++ : it++;
            ptr_it = !ptr_it;
            continue;
        }
    }

    // copy the left overs;
    // happens only if the ending times are different for the pending samples
    if(ptr_it ? it != audio_sample.buffer->samples.end() : jt != audio_sample2.buffer->samples.end())
    {
        LONGLONG t, t2, dur, dur2;
        CHECK_HR(hr = audio_sample.buffer->samples.back()->GetSampleTime(&t));
        CHECK_HR(hr = audio_sample.buffer->samples.back()->GetSampleDuration(&dur));
        CHECK_HR(hr = audio_sample2.buffer->samples.back()->GetSampleTime(&t2));
        CHECK_HR(hr = audio_sample2.buffer->samples.back()->GetSampleDuration(&dur2));
        /*if(std::abs((t + dur) - (t2 + dur2)) <= 2)
        {
            std::cout << "COPY SKIPPED" << std::endl;
            goto skip;
        }*/

        std::cout << "COPY" << std::endl;

        auto end = ptr_it ? audio_sample.buffer->samples.end() : audio_sample2.buffer->samples.end();
        for(auto kt = ptr_it ? it : jt; kt != end; kt++)
        {
            CComPtr<IMFSample> sample;
            CComPtr<IMFMediaBuffer> buffer;
            frame_unit ts, dur, tp_start;

            CHECK_HR(hr = (*kt)->GetSampleTime(&ts));
            CHECK_HR(hr = (*kt)->GetSampleDuration(&dur));
            tp_start = std::max(ptr_tp_start, ts);

            CHECK_HR(hr = MFCreateSample(&sample));
            buffer = this->transform->copy(
                audio_sample.bit_depth, audio_sample.channels,
                *kt, tp_start, ts + dur);
            CHECK_HR(hr = sample->AddBuffer(buffer));
            CHECK_HR(hr = sample->SetSampleTime(tp_start));
            CHECK_HR(hr = sample->SetSampleDuration(ts + dur - tp_start));
            audio.buffer->samples.push_back(sample);
        }
    }
skip:

    // currently the samples themselves in the buffer contain timestamps
    audio.timestamp = std::min(audio_sample.timestamp, audio_sample2.timestamp);

    this->pending_request.rp = request_packet();
    this->pending_request2.rp = request_packet();
    this->pending_request.rp.packet_number = INVALID_PACKET_NUMBER;
    this->pending_request2.rp.packet_number = INVALID_PACKET_NUMBER;

    this->transform->session->give_sample(this, audio, rp, false);

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

media_stream::result_t stream_audiomix::request_sample(request_packet& rp, const media_stream*)
{
    return this->transform->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_audiomix::process_sample(
    const media_sample& sample_view, request_packet& rp, const media_stream* prev_stream)
{
    std::unique_lock<std::mutex> lock(this->mutex);

    const media_sample_audio& audio_sample = static_cast<const media_sample_audio&>(sample_view);

    if(prev_stream == this->primary_stream)
    {
        assert_(this->pending_request.rp.packet_number == INVALID_PACKET_NUMBER);

        this->pending_request.rp = rp;
        this->pending_request.audio_sample = audio_sample;
    }
    else
    {
        assert_(this->pending_request2.rp.packet_number == INVALID_PACKET_NUMBER);

        this->pending_request2.rp = rp;
        this->pending_request2.audio_sample = audio_sample;
    }

    if(this->pending_request.rp.packet_number != INVALID_PACKET_NUMBER &&
        this->pending_request2.rp.packet_number != INVALID_PACKET_NUMBER)
    {
        lock.unlock();
        this->process_cb(NULL);
    }

    return OK;
}