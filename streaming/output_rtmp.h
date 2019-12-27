#pragma once

#include "output_class.h"
#include "media_sample.h"
#include "wtl.h"
#include <memory>
#include <deque>
#include <string>
#include <string_view>
#include <mutex>

#define RECORDING_STOPPED_MESSAGE (WM_APP + 1)

// TODO: write the rest of the packets in destructor(end of sequence could be sent by using flv packets)
// TODO: bitrate should be stabilized by adding filler nalus to the stream

struct RTMP;

class output_rtmp final : public output_class
{
public:
    using scoped_lock = std::lock_guard<std::mutex>;
private:
    CWindow recording_initiator;
    RTMP* rtmp;
    CComPtr<IMFMediaType> video_type;
    CComPtr<IMFMediaType> audio_type;
    std::mutex write_lock;

    std::deque<CComPtr<IMFSample>> video_samples, audio_samples;

    std::string sps_nalu, pps_nalu;
    bool video_headers_sent, audio_headers_sent;

    std::string create_avc_decoder_configuration_record(
        const std::string_view& sps_nalu, const std::string_view& pps_nalu,
        int start_code_prefix_len) const;
    std::string create_audio_specific_config() const;
    static std::size_t find_start_code_prefix(const std::string_view&, int& start_code_prefix_len);

    // pts and dts are in 100 nanosecond units
    void send_rtmp_video_packets(const std::string_view&, LONGLONG pts, LONGLONG dts, bool key_frame);
    void send_rtmp_audio_packets(const std::string_view&, LONGLONG ts);
    void send_flv_metadata();

    void send_rtmp_packets();
public:
    output_rtmp();
    ~output_rtmp();

    void initialize(
        const std::string_view& url,
        const std::string_view& streaming_key,
        CWindow recording_initiator,
        const CComPtr<IMFMediaType>& video_type,
        const CComPtr<IMFMediaType>& audio_type);

    void write_sample(bool video, const CComPtr<IMFSample>& sample) override;
};

using output_rtmp_t = std::shared_ptr<output_rtmp>;