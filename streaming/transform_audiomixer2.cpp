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
    buffer_pool_audio_frames(new buffer_pool_audio_frames_t),
    buffer_pool_audio_mixer_frames(new buffer_pool_audio_mixer_frames_t)
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
    {
        buffer_pool_audio_mixer_frames_t::scoped_lock 
            lock(this->buffer_pool_audio_mixer_frames->mutex);
        this->buffer_pool_audio_mixer_frames->dispose();
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

bool stream_audiomixer2::move_frames(in_arg_t& to, in_arg_t& from, const in_arg_t& reference,
    frame_unit end, bool discarded)
{
    assert_(reference);

    to = std::make_optional<in_arg_t::value_type>();
    from = std::make_optional<in_arg_t::value_type>();

    to->frame_end = end;
    from->frame_end = reference->frame_end;

    if(reference->sample)
    {
        {
            transform_audiomixer2::buffer_pool_audio_mixer_frames_t::scoped_lock lock(
                this->transform->buffer_pool_audio_mixer_frames->mutex);
            from->sample = this->transform->buffer_pool_audio_mixer_frames->acquire_buffer();
            from->sample->initialize();
        }

        // *from->sample = *reference->sample
        // could be used, but currently the fields are assigned individually
        from->sample->frames = reference->sample->frames;
        from->sample->end = reference->sample->end;

        if(!discarded)
        {
            transform_audiomixer2::buffer_pool_audio_mixer_frames_t::scoped_lock lock(
                this->transform->buffer_pool_audio_mixer_frames->mutex);
            to->sample = this->transform->buffer_pool_audio_mixer_frames->acquire_buffer();
            to->sample->initialize();
        }

        const bool moved = from->sample->move_frames_to(to->sample.get(), end,
            transform_audiomixer2::block_align);
        if(moved && discarded)
            std::cout << "discarded audio frames" << std::endl;
    }
    if(end >= from->frame_end)
        from.reset();

    // reset the samples of 'from' and 'to' if they contain no data,
    // because frame collections with empty data is not currently allowed
    if(from && from->sample && from->sample->frames.empty())
        from->sample.reset();
    if(to && to->sample && to->sample->frames.empty())
        to->sample.reset();

    return !from;
}

void stream_audiomixer2::mix(out_arg_t& out_arg, args_t& packets,
    frame_unit first, frame_unit end)
{
    assert_(!packets.container.empty());
    assert_(!out_arg);

    // arg in packets can be null when frame_count == 0 or
    // if a source passed null args on a drain point;
    // the whole leftover buffer is merged to packets

    // TODO: audio mixer should only allocate buffers for frame positions that are 
    // present in the input samples;
    // currently, audio mixer simply allocates a buffer size of (end - first),
    // which has a side effect of audiomixer possibly generating new data - a property that
    // should be only restricted to sources where the sample data generation has an upper limit
    // (audio mixer should work similarly to video mixer)

    HRESULT hr = S_OK;
    const UINT32 out_block_align = 
        transform_aac_encoder::bit_depth / 8 * transform_aac_encoder::channels;
    typedef transform_audiomixer2::bit_depth_t in_bit_depth_t;
    typedef transform_aac_encoder::bit_depth_t out_bit_depth_t;

    const frame_unit frame_count = end - first;
    media_sample_audio_frames_t frames;
    media_buffer_memory_t out_buffer;
    const DWORD out_buffer_len = (UINT32)frame_count * out_block_align;

    assert_(frame_count >= 0);
    if(frame_count == 0)
        return;

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
        // null arg happens only if a source passed null args on drain;
        // arg with empty sample indicates a frame skip;
        // frame with empty buffer indicates a silent frame
        if(!item.arg || !item.arg->sample)
            continue;

        assert_(end >= item.arg->sample->end);
        // empty frame collection isn't allowed
        assert_(!item.arg->sample->frames.empty());
        for(auto&& consec_frames : item.arg->sample->frames)
        {
            assert_(first <= consec_frames.pos);
            assert_(end >= (consec_frames.pos + consec_frames.dur));

            if(!consec_frames.buffer)
                continue;

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