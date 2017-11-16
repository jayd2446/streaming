#pragma once

#include "media_source.h"
#include "media_stream.h"
#include <mfapi.h>
#include <memory>
#include <mutex>

#pragma comment(lib, "Mfplat.lib")

class stream_audiomix;
typedef std::shared_ptr<stream_audiomix> stream_audiomix_t;

// both streams need to have same sample rate, same bit depth
// and same amount of channels
class transform_audiomix : public media_source
{
    friend class stream_audiomix;
private:
    // both might return null
    CComPtr<IMFMediaBuffer> copy(
        UINT32 bit_depth, UINT32 channels,
        const CComPtr<IMFSample>& sample, frame_unit start, frame_unit end) const;
    CComPtr<IMFMediaBuffer> mix(
        UINT32 bit_depth, UINT32 channels,
        const CComPtr<IMFSample>& sample,
        const CComPtr<IMFSample>& sample2,
        frame_unit start, frame_unit end) const;
public:
    explicit transform_audiomix(const media_session_t& session);
    stream_audiomix_t create_stream();
};

typedef std::shared_ptr<transform_audiomix> transform_audiomix_t;

class stream_audiomix : public media_stream
{
public:
    struct packet {request_packet rp; media_sample_view_t sample_view;};
private:
    transform_audiomix_t transform;
    packet pending_request, pending_request2;
    CComPtr<IMFSample> output_sample;
    std::mutex mutex;
    const media_stream* primary_stream;

    void process_cb(void*);
public:
    explicit stream_audiomix(const transform_audiomix_t& transform);

    void set_primary_stream(const media_stream* stream) {this->primary_stream = stream;}

    bool get_clock(presentation_clock_t& c) {return this->transform->session->get_current_clock(c);}
    result_t request_sample(request_packet&, const media_stream*);
    result_t process_sample(const media_sample_view_t&, request_packet&, const media_stream*);
};