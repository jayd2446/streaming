#include "transform_audioprocessor.h"

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

transform_audioprocessor::transform_audioprocessor(const media_session_t& session) :
    media_source(session)
{
}

media_stream_t transform_audioprocessor::create_stream()
{
    return media_stream_t(new stream_audioprocessor(this->shared_from_this<transform_audioprocessor>()));
}

void transform_audioprocessor::channel_convert(media_buffer_samples_t& samples)
{
    HRESULT hr = S_OK;
    /*const UINT32 block_align = samples->bit_depth * samples->channels;*/
    const int new_channels = 2;
    typedef int16_t bit_depth_t;

    if(samples->channels == new_channels)
        return;

    for(auto it = samples->samples.begin(); it != samples->samples.end(); it++)
    {
        DWORD buflen;
        CComPtr<IMFMediaBuffer> buffer, out_buffer;
        BYTE* data_in, *data_out;
        CHECK_HR(hr = (*it)->GetBufferByIndex(0, &buffer));
        CHECK_HR(hr = buffer->GetCurrentLength(&buflen));
        CHECK_HR(hr = MFCreateMemoryBuffer(buflen * new_channels, &out_buffer));
        CHECK_HR(hr = out_buffer->SetCurrentLength(buflen * new_channels));

        CHECK_HR(hr = buffer->Lock(&data_in, NULL, NULL));
        CHECK_HR(hr = out_buffer->Lock(&data_out, NULL, NULL));
        for(UINT32 i = 0; i < buflen / samples->bit_depth; i++)
        {
            ((bit_depth_t*)data_out)[i * 2] = ((bit_depth_t*)data_in)[i];
            ((bit_depth_t*)data_out)[i * 2 + 1] = ((bit_depth_t*)data_in)[i];
        }
        CHECK_HR(hr = out_buffer->Unlock());
        CHECK_HR(hr = buffer->Unlock());

        CHECK_HR(hr = (*it)->RemoveAllBuffers());
        CHECK_HR(hr = (*it)->AddBuffer(out_buffer));
    }

    samples->channels = 2;

done:
    if(FAILED(hr))
        throw std::exception();
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_audioprocessor::stream_audioprocessor(const transform_audioprocessor_t& transform) :
    transform(transform)
{
}

media_stream::result_t stream_audioprocessor::request_sample(request_packet& rp, const media_stream*)
{
    return this->transform->session->request_sample(this, rp, false) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_audioprocessor::process_sample(
    const media_sample_view_t& sample_view, request_packet& rp, const media_stream*)
{
    if(sample_view)
        this->transform->channel_convert(sample_view->get_buffer<media_buffer_samples>());

    this->transform->session->give_sample(this, sample_view, rp, false);
    return OK;
}