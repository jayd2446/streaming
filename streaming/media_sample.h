#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <deque>
#include <stack>
#include <utility>
#include <optional>
#include <type_traits>
#include <condition_variable>
#include <functional>
#include <stdint.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <atlbase.h>
#include <mfidl.h>
#include "assert.h"
#include "enable_shared_from_this.h"
#include "buffer_pool.h"

#undef max
#undef min

#define SECOND_IN_TIME_UNIT 10000000

extern const GUID media_sample_lifetime_tracker_guid;

// 100 nanosecond = 1 time_unit
typedef int64_t time_unit;
// frame unit is used to accurately represent a frame position
// relative to the time source
typedef int64_t frame_unit;

frame_unit convert_to_frame_unit(time_unit, frame_unit frame_rate_num, frame_unit frame_rate_den);
time_unit convert_to_time_unit(frame_unit, frame_unit frame_rate_num, frame_unit frame_rate_den);

// TODO: remove this
class media_buffer : public enable_shared_from_this
{
public:
    /*CComPtr<IUnknown> lifetime_tracker;*/

    virtual ~media_buffer() {}
};

typedef std::shared_ptr<media_buffer> media_buffer_t;

// h264 memory buffer
// TODO: just use media_buffer_samples and remove h264&aac buffers
class media_buffer_h264 : public media_buffer
{
public:
    std::deque<CComPtr<IMFSample>> samples;
    /*CComPtr<IMFSample> sample;*/
    virtual ~media_buffer_h264() {}
};

typedef std::shared_ptr<media_buffer_h264> media_buffer_h264_t;

class media_buffer_texture : public buffer_poolable
{
    friend class buffer_pooled<media_buffer_texture>;
private:
    // the buffer should be always initialized with the same desc and device
    void uninitialize() {}
public:
    // TODO: the memory buffer should be a derived class of this defined in h264 encoder;
    // rename the texture_buffer to memory_buffer
    DWORD texture_buffer_length;
    std::unique_ptr<BYTE[]> texture_buffer;

    // TODO: this bitmap should be set in displaycapture aswell so that videomixer
    // doesn't need to create it for every input texture
    CComPtr<ID2D1Bitmap1> bitmap;
    CComPtr<ID3D11Texture2D> texture;
    virtual ~media_buffer_texture() {}

    // currently, initialize doesn't initialize the bitmap
    void initialize(const CComPtr<ID3D11Device>&,
        const D3D11_TEXTURE2D_DESC&, const D3D11_SUBRESOURCE_DATA*);
};

typedef std::shared_ptr<media_buffer_texture> media_buffer_texture_t;
typedef buffer_pooled<media_buffer_texture> media_buffer_pooled_texture;
typedef std::shared_ptr<media_buffer_pooled_texture> media_buffer_pooled_texture_t;

// set to imfsample to ensure that the sample isn't recycled before imfsample has been released;
// the tracker must be manually removed from the sample
CComPtr<IUnknown> create_lifetime_tracker(const media_buffer_t&);

class media_buffer_memory : public buffer_poolable
{
    friend class buffer_pooled<media_buffer_memory>;
private:
    void uninitialize() {}
public:
    // the buffer is readonly
    CComPtr<IMFMediaBuffer> buffer;

    virtual ~media_buffer_memory() {}

    // methods for poolable samples;
    // alignment isn't passed; it should be the lowest common dividor
    void initialize(DWORD len);
};

typedef std::shared_ptr<media_buffer_memory> media_buffer_memory_t;
typedef buffer_pooled<media_buffer_memory> media_buffer_memory_pooled;
typedef std::shared_ptr<media_buffer_memory_pooled> media_buffer_memory_pooled_t;

class media_sample_audio_consecutive_frames
{
public:
    frame_unit pos, dur;
    // a wrapper of the original buffer(or the original buffer)
    CComPtr<IMFMediaBuffer> buffer;
    media_buffer_memory_t memory_host;
};

class media_sample_audio_frames : public buffer_poolable
{
    friend class buffer_pooled<media_sample_audio_frames>;
private:
    // called when the buffer is moved back to pool and just before being destroyed
    // TODO: decide if should call reserve here
    void uninitialize() {this->frames.clear(); this->end = 0;}
public:
    // end must be 0 if there's no valid data(TODO: reconsider this);
    // end is the max (pos + dur) of frames
    frame_unit end;
    // element must have valid data
    // TODO: make this private so that the end field stays consistent in regard to frames
    std::deque<media_sample_audio_consecutive_frames> frames;

    media_sample_audio_frames() : end(0) {}
    virtual ~media_sample_audio_frames() {}

    // moves part from this to 'to'
    // returns whether any frames were moved;
    // to can be null, in which case the contents from this are discarded;
    // std move is used for conventional move where the contents of this are replaced;
    // std::numeric_limits::max can be used for moving all frames;
    // block align is assumed to be the same for both samples
    bool move_frames_to(media_sample_audio_frames* to, frame_unit end, UINT32 block_align);

    // buffer pool methods
    void initialize() {assert_(this->end == 0); assert_(this->frames.empty());}
};

typedef std::shared_ptr<media_sample_audio_frames> media_sample_audio_frames_t;
typedef buffer_pooled<media_sample_audio_frames> media_sample_audio_frames_pooled;
typedef std::shared_ptr<media_sample_audio_frames_pooled> media_sample_audio_frames_pooled_t;

class media_sample_aac_frame
{
public:
    // time unit is used because the media foundation aac encoder returns timestamps
    time_unit ts, dur;
    CComPtr<IMFMediaBuffer> buffer;
    media_buffer_memory_t memory_host;
};

class media_sample_aac_frames : public buffer_poolable
{
    // this is very similar to media_sample_audio_frames
    friend class buffer_pooled<media_sample_aac_frames>;
private:
    // called when the buffer is moved back to pool and just before being destroyed
    // TODO: decide if should call reserve here
    void uninitialize() {this->frames.clear();}
public:
    std::deque<media_sample_aac_frame> frames;

    virtual ~media_sample_aac_frames() {}

    // buffer pool methods
    void initialize() {assert_(this->frames.empty());}
};

typedef std::shared_ptr<media_sample_aac_frames> media_sample_aac_frames_t;
typedef buffer_pooled<media_sample_aac_frames> media_sample_aac_frames_pooled;
typedef std::shared_ptr<media_sample_aac_frames_pooled> media_sample_aac_frames_pooled_t;

class media_buffer_textures : public media_buffer
{
public:
    // every element must be valid
    std::deque<std::pair<frame_unit /*frame pos*/, media_buffer_texture_t>> frames;
    virtual ~media_buffer_textures() {}
};

typedef std::shared_ptr<media_buffer_textures> media_buffer_textures_t;

//    // TODO: the session could have properties, which would include the frame rate(or the clock);
//    // additional properties(canvas resolution etc) could be accessed from the control pipeline;
//    // for audio, channel count and bit depth should be bound to media session aswell for
//    // simplicity

class media_sample_h264
{
public:
    typedef media_buffer_h264_t buffer_t;
public:
    // for debugging
    time_unit timestamp;
    bool software;
    media_buffer_h264_t buffer;
    media_sample_h264() : timestamp(-1) {}
    explicit media_sample_h264(const media_buffer_h264_t& buffer);
    virtual ~media_sample_h264() {}
};

class media_component_args
{
public:
};

// arg type for components that expect uncompressed audio or video samples
class media_component_frame_args : public media_component_args
{
public:
    // frame_end is greater or equal to the end of the data in the sample
    frame_unit frame_end;
};

typedef std::optional<media_component_frame_args> media_component_frame_args_t;

class media_component_video_args : public media_component_frame_args
{
public:
    // TODO: rename buffers to samples
    media_buffer_textures_t buffer;
    // single buffer can be used instead of multiple buffers
    // TODO: remove this
    media_buffer_texture_t single_buffer;

    media_component_video_args() = default;
    explicit media_component_video_args(const media_buffer_texture_t& single_buffer) :
        single_buffer(single_buffer) {}
};

typedef std::optional<media_component_video_args> media_component_video_args_t;

class media_component_audio_args : public media_component_frame_args
{
public:
    media_sample_audio_frames_t sample;
};

typedef std::optional<media_component_audio_args> media_component_audio_args_t;

// aac encoder expects that the frame end equals to the sample end
class media_component_aac_encoder_args : public media_component_frame_args
{
public:
    // must not be null
    media_sample_audio_frames_t sample;
    bool is_valid() const {return (this->sample && this->sample->end == this->frame_end);}
};

typedef std::optional<media_component_aac_encoder_args> media_component_aac_encoder_args_t;

// args for components that expect aac audio data
class media_component_aac_audio_args : public media_component_args
{
public:
    // must not be null
    media_sample_aac_frames_t sample;
};

typedef std::optional<media_component_aac_audio_args> media_component_aac_audio_args_t;