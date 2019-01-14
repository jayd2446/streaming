#include "transform_audiomixer2.h"
#include "transform_aac_encoder.h"
#include "assert.h"
#include <Mferror.h>
#include <iostream>
#include <limits>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
#undef min
#undef max

transform_audiomixer2::transform_audiomixer2(const media_session_t& session) :
    transform_audiomixer2_base(session),
    buffer_pool_memory(new buffer_pool_memory_t),
    buffer_pool_audio_frames(new buffer_pool_audio_frames_t)
{
    
}

transform_audiomixer2::~transform_audiomixer2()
{
    {
        buffer_pool_memory_t::scoped_lock lock(this->buffer_pool_memory->mutex);
        this->buffer_pool_memory->dispose();
    }
    {
        buffer_pool_audio_frames_t::scoped_lock lock(this->buffer_pool_audio_frames->mutex);
        this->buffer_pool_audio_frames->dispose();
    }
}

void transform_audiomixer2::initialize()
{
    transform_audiomixer2_base::initialize(transform_aac_encoder::sample_rate, 1);
}

transform_audiomixer2::stream_mixer_t transform_audiomixer2::create_derived_stream()
{
    return stream_audiomixer2_base_t(
        new stream_audiomixer2(this->shared_from_this<transform_audiomixer2>()));
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_audiomixer2::stream_audiomixer2(const transform_audiomixer2_t& transform) :
    stream_mixer(transform),
    transform(transform)
{
}

bool stream_audiomixer2::move_frames(in_arg_t& in_arg, in_arg_t& old_in_arg, frame_unit end,
    bool discarded)
{
    assert_(old_in_arg);
    // TODO: video mixer should update the fields for both sample and old sample aswell
    in_arg = std::make_optional<in_arg_t::value_type>();

    media_sample_audio_frames_t frames;
    if(!discarded)
    {
        transform_audiomixer2::buffer_pool_audio_frames_t::scoped_lock lock(
            this->transform->buffer_pool_audio_frames->mutex);
        frames = this->transform->buffer_pool_audio_frames->acquire_buffer();
        frames->initialize();
    }

    // set old_in_arg
    if(old_in_arg->sample)
    {
        const bool moved = old_in_arg->sample->move_frames_to(frames.get(), end,
            transform_audiomixer2::bit_depth / 8 * transform_aac_encoder::channels);

        if(moved && discarded)
            std::cout << "discarded frames" << std::endl;
    }
    if(end >= old_in_arg->frame_end)
        // make the old_sample null
        old_in_arg.reset();

    // set in_arg
    in_arg->frame_end = end;
    in_arg->sample = frames;

    return !old_in_arg;
}

void stream_audiomixer2::mix(out_arg_t& out_arg, args_t& packets,
    frame_unit first, frame_unit end)
{
    // the samples in packets might be null
    assert_(!packets.container.empty());
    assert_(first <= end);
    assert_(!out_arg);

    HRESULT hr = S_OK;
    const UINT32 out_block_align = 
        transform_aac_encoder::bit_depth / 8 * transform_aac_encoder::channels;
    /*const UINT32 in_block_align =
        transform_audiomixer2::bit_depth / 8 * transform_aac_encoder::channels;*/
    typedef transform_audiomixer2::bit_depth_t in_bit_depth_t;
    typedef transform_aac_encoder::bit_depth_t out_bit_depth_t;

    const frame_unit frame_count = end - first;
    media_sample_audio_frames_t frames;
    media_buffer_memory_t out_buffer;
    const DWORD out_buffer_len = (UINT32)frame_count * out_block_align;

    if(frame_count > 0)
    {
        {
            transform_audiomixer2::buffer_pool_audio_frames_t::scoped_lock lock(
                this->transform->buffer_pool_audio_frames->mutex);
            frames = this->transform->buffer_pool_audio_frames->acquire_buffer();
            frames->initialize();
        }
        {
            transform_audiomixer2::buffer_pool_memory_t::scoped_lock lock(
                this->transform->buffer_pool_memory->mutex);
            out_buffer = this->transform->buffer_pool_memory->acquire_buffer();
            out_buffer->initialize(out_buffer_len);
        }

        out_bit_depth_t* out_data_base;
        CHECK_HR(hr = out_buffer->buffer->SetCurrentLength(out_buffer_len));
        CHECK_HR(hr = out_buffer->buffer->Lock((BYTE**)&out_data_base, NULL, NULL));

        memset(out_data_base, 0, out_buffer_len);
        for(auto&& item : packets.container)
        {
            if(!item.arg || !item.arg->sample)
                continue;

            assert_(end >= item.arg->sample->end);
            for(auto&& consec_frames : item.arg->sample->frames)
            {
                assert_(first <= consec_frames.pos);
                assert_(end >= (consec_frames.pos + consec_frames.dur));

                in_bit_depth_t* in_data;
                out_bit_depth_t* out_data = out_data_base;
                CHECK_HR(hr = consec_frames.buffer->Lock((BYTE**)&in_data, 0, 0));
                
                out_data += (UINT32)(consec_frames.pos - first) * transform_aac_encoder::channels;
                for(UINT32 i = 0; 
                    i < (UINT32)consec_frames.dur * transform_aac_encoder::channels; 
                    i++)
                {
                    static_assert(std::is_floating_point<transform_audiomixer2::bit_depth_t>::value, 
                        "float type expected");

                    int64_t temp = *out_data;
                    temp += (int64_t)(*in_data++ * 
                        std::numeric_limits<transform_aac_encoder::bit_depth_t>::max());

                    // clamp
                    *out_data++ = (out_bit_depth_t)std::max(
                        (int64_t)std::numeric_limits<out_bit_depth_t>::min(),
                        std::min(temp, (int64_t)std::numeric_limits<out_bit_depth_t>::max()));
                }

                CHECK_HR(hr = consec_frames.buffer->Unlock());
            }
        }

        CHECK_HR(hr = out_buffer->buffer->Unlock());

        {
            assert_(frames->frames.empty());
            media_sample_audio_consecutive_frames consec_frames;
            consec_frames.memory_host = out_buffer;
            consec_frames.buffer = out_buffer->buffer;
            consec_frames.pos = first;
            consec_frames.dur = frame_count;
            frames->end = end;
            frames->frames.push_back(consec_frames);
        }

        assert_(end > 0);
        out_arg = std::make_optional<out_arg_t::value_type>();
        out_arg->sample = std::move(frames);
        out_arg->frame_end = end;
    }

    // out_arg will be null if frame_count was 0

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


void stream_audiomixer2_controller::get_params(params_t& params) const
{
    scoped_lock lock(this->mutex);
    params = this->params;
}

void stream_audiomixer2_controller::set_params(const params_t& params)
{
    scoped_lock lock(this->mutex);
    this->params = params;
}