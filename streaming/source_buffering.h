#pragma once
#include "source_base.h"
#include "media_component.h"
#include "media_stream.h"
#include "transform_videomixer.h"
#include "transform_audiomixer2.h"
#include <memory>

// forces transform_mixer to buffer samples by serving samples by a predefined latency;
// it is used to make sure that a topology switch to sources with different latencies
// won't cause frame drops;
// it also stabilizes the processing interval of samples in mixing and encoding stages

template<class OutArgs>
class stream_buffering;

template<class OutArgs>
class source_buffering final : public source_base<OutArgs>
{
    friend class stream_buffering<OutArgs>;
public:
    typedef OutArgs out_args_t;
    typedef stream_buffering<out_args_t> stream_buffering;
    typedef std::shared_ptr<stream_buffering> stream_buffering_t;
    typedef typename source_base<OutArgs>::stream_source_base_t stream_source_base_t;
private:
    time_unit latency;

    stream_source_base_t create_derived_stream() override;
    bool get_samples_end(time_unit request_time, frame_unit& end) const override;
    void make_request(request_t&, frame_unit frame_end) override;
    void dispatch(request_t&) override;
public:
    explicit source_buffering(const media_session_t& session);

    void initialize(const control_class_t&, time_unit latency);
};

typedef source_buffering<media_component_audiomixer_args> source_buffering_audio;
typedef source_buffering<media_component_videomixer_args> source_buffering_video;
typedef std::shared_ptr<source_buffering_audio> source_buffering_audio_t;
typedef std::shared_ptr<source_buffering_video> source_buffering_video_t;

template<class OutArgs>
class stream_buffering : public stream_source_base<source_base<OutArgs>>
{
public:
    typedef OutArgs out_args_t;
    typedef source_buffering<out_args_t> source_buffering;
    typedef std::shared_ptr<source_buffering> source_buffering_t;
private:
    source_buffering_t source;
public:
    explicit stream_buffering(const source_buffering_t&);
};

typedef stream_buffering<media_component_audiomixer_args> stream_buffering_audio;
typedef stream_buffering<media_component_videomixer_args> stream_buffering_video;
typedef std::shared_ptr<stream_buffering_audio> stream_buffering_audio_t;
typedef std::shared_ptr<stream_buffering_video> stream_buffering_video_t;


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<class T>
source_buffering<T>::source_buffering(const media_session_t& session) : source_base<T>(session)
{
}

template<class T>
void source_buffering<T>::initialize(const control_class_t& ctrl_pipeline, time_unit latency)
{
    this->source_base<T>::initialize(ctrl_pipeline);
    this->latency = latency;
}

template<class T>
typename source_buffering<T>::stream_source_base_t source_buffering<T>::create_derived_stream()
{
    return stream_buffering_t(new stream_buffering(this->shared_from_this<source_buffering>()));
}

template<class T>
bool source_buffering<T>::get_samples_end(time_unit /*request_time*/, frame_unit& end) const
{
    media_clock_t clock = this->session->get_clock();
    end = convert_to_frame_unit(clock->get_current_time() - this->latency,
        this->session->frame_rate_num, this->session->frame_rate_den);
    return true;
}

template<class T>
void source_buffering<T>::make_request(request_t& request, frame_unit frame_end)
{
    request.sample.args = std::make_optional<out_args_t>();
    out_args_t& args = *request.sample.args;
    args.frame_end = frame_end;
}

template<class T>
void source_buffering<T>::dispatch(request_t& request)
{
    this->session->give_sample(request.stream, request.sample.args.has_value() ?
        &(*request.sample.args) : NULL, request.rp);
}


///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////


template<class T>
stream_buffering<T>::stream_buffering(const source_buffering_t& source) :
    stream_source_base<::source_base<T>>(source),
    source(source)
{
}