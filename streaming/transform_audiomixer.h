#pragma once

#include "media_source.h"
#include "media_stream.h"
#include "request_packet.h"
#include "transform_aac_encoder.h"
#include <mfapi.h>
#include <memory>
#include <mutex>
#include <utility>
#include <list>

#pragma comment(lib, "Mfplat.lib")

class stream_audiomixer;
typedef std::shared_ptr<stream_audiomixer> stream_audiomixer_t;

// both streams need to have same sample rate, same bit depth
// and same amount of channels
class transform_audiomixer : public media_source
{
    friend class stream_audiomixer;
public:
    typedef std::lock_guard<std::mutex> scoped_lock;
    typedef std::list<media_sample_audio> media_sample_audios;
    struct packet 
    {
        media_sample_audios audios; 
        bool drain;

        // move must be explicitly defined for this plain struct
        // because vs 2013 is out dated
        packet() {}
        packet(packet&& other) : audios(std::move(other.audios)), drain(other.drain) {}
        packet& operator=(packet&& other) 
        {this->audios = std::move(other.audios); this->drain = other.drain; return *this;}
    };
    typedef request_queue<packet> request_queue;
    typedef request_queue::request_t request_t;

    /*struct packet {request_packet rp; media_sample_audio audio_sample;};*/
    typedef transform_aac_encoder::bit_depth_t bit_depth_t;
private:
    std::mutex process_mutex;
    frame_unit /*next_position, next_position2,*/ next_position3;
    frame_unit out_next_position;
    bool next_position_initialized;
    media_sample_audio leftover_audio;

    request_queue requests;

    void try_initialize_next_positions(time_unit request_time);

    // returns whether the whole buffer was mixed to out
    bool mix(media_buffer_samples&, const media_sample_audio&, frame_unit& next_position);
    void process(media_sample_audio& audio, 
        const media_sample_audio&, const media_sample_audio&, int input_stream_count, bool drain);

    void process(media_sample_audio& audio, const media_sample_audios&, bool drain);

    void process();
public:
    explicit transform_audiomixer(const media_session_t& session);

    void initialize() {}
    stream_audiomixer_t create_stream(presentation_clock_t&);
};

typedef std::shared_ptr<transform_audiomixer> transform_audiomixer_t;

// request queue packet contains both audios(request queue is used for audio mixer and master audio mixer)

// TODO: decide if mix the audio here and add it to the request queue
// or the mix the audio and pass it to downstream; master audio mixer will
// buffer the leftover audio; master audio mixer only mixes the
// input audio to silence buffer and buffers leftover
class stream_audiomixer : public media_stream_clock_sink
{
private:
    transform_audiomixer_t transform;
    transform_audiomixer::request_t request;
    transform_audiomixer::media_sample_audios audios;
    std::mutex mutex;
    int input_stream_count, samples_received;

    time_unit drain_point;

    void on_component_start(time_unit);
    void on_component_stop(time_unit);
public:
    explicit stream_audiomixer(const transform_audiomixer_t& transform);

    /*void set_primary_stream(const media_stream* stream) {this->primary_stream = stream;}*/

    void connect_streams(const media_stream_t& from, const media_topology_t&);
    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample&, request_packet&, const media_stream*);
};