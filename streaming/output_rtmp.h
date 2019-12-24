#pragma once

#include "output_class.h"
#include "media_sample.h"
#include "wtl.h"
#include <librtmp/rtmp.h>
#include <memory>
#include <vector>
#include <string>
#include <string_view>

#define RECORDING_STOPPED_MESSAGE (WM_APP + 1)

class output_rtmp final : public output_class
{
private:
    CWindow recording_initiator;
    RTMP* rtmp;
    CComPtr<IMFMediaType> video_type;
    CComPtr<IMFMediaType> audio_type;

    std::string sps_nalu, pps_nalu;
    bool video_headers_sent, audio_headers_sent;

    std::string create_avc_decoder_configuration_record(
        const std::string_view& sps_nalu, const std::string_view& pps_nalu,
        int start_code_prefix_len) const;
    std::string create_audio_specific_config() const;

    // returns the index to first occurence, npos if not found;
    // might throw out of range;
    // the start code prefix is only found in annex b h264 elementary streams;
    // in avcc streams nal unit size is used at the same position
    static std::size_t find_start_code_prefix(const std::string_view&, int& start_code_prefix_len);
    // pts and dts are in 100 nanosecond units
    void send_rtmp_video_packets(const std::string_view&, LONGLONG pts, LONGLONG dts, bool key_frame);
    void send_rtmp_audio_packets(const std::string_view&, LONGLONG ts);

    // returns false when all meta data is sent
    bool send_flv_metadata();
public:
    output_rtmp();
    ~output_rtmp();

    void initialize(
        const char* url,
        CWindow recording_initiator,
        const CComPtr<IMFMediaType>& video_type,
        const CComPtr<IMFMediaType>& audio_type);

    void write_sample(bool video, frame_unit fps_num, frame_unit fps_den,
        const CComPtr<IMFSample>& sample) override;
};

using output_rtmp_t = std::shared_ptr<output_rtmp>;