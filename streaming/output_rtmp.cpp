#include <librtmp/rtmp.h>
#include <librtmp/log.h>
#include <librtmp/amf.h>

#include "output_rtmp.h"
#include <codecapi.h>
#include <intrin.h>
#include <iostream>
#include <limits>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}
#undef min
#undef max

#pragma pack(push, 1)
struct flv_tag
{
    uint8_t tag_type;
    uint8_t data_size[3];
    uint8_t timestamp[3];
    uint8_t timestamp_extended; // represents the upper 8 bits of the timestamp
    uint8_t stream_id[3]; // always 0
};
#pragma pack(pop)

output_rtmp::output_rtmp() : 
    rtmp(nullptr),
    video_headers_sent(false), audio_headers_sent(false)
{
}

output_rtmp::~output_rtmp()
{
    if(this->rtmp)
    {
        RTMP_Close(this->rtmp);
        RTMP_Free(this->rtmp);
    }

    this->recording_initiator.SendNotifyMessageW(RECORDING_STOPPED_MESSAGE, 0);
}

void output_rtmp::send_flv_metadata()
{
    // sends a scriptdata tag named onMetaData

    assert_(RTMP_IsConnected(this->rtmp));

    HRESULT hr = S_OK;

    constexpr std::string_view onMetaData_str = "onMetaData";
    constexpr int elem_count = 20;
    constexpr std::string_view encoder = "obs-output module (libobs version 24.0.6)";

    auto enc_num_val = [](char*& p, char* end, const std::string_view& name, double val)
    {
        const AVal s = {const_cast<char*>(name.data()), (int)name.size()};
        p = AMF_EncodeNamedNumber(p, end, &s, val);
    };
    auto enc_str_val = [](char*& p, char* end, const std::string_view& name,
        const std::string_view& val)
    {
        const AVal s = {const_cast<char*>(name.data()), (int)name.size()},
            s2 = {const_cast<char*>(val.data()), (int)val.size()};
        p = AMF_EncodeNamedString(p, end, &s, &s2);
    };
    auto enc_bool_val = [](char*& p, char* end, const std::string_view& name, bool val)
    {
        const AVal s = {const_cast<char*>(name.data()), (int)name.size()};
        p = AMF_EncodeNamedBoolean(p, end, &s, val);
    };

    char buffer[4096];
    char* p = buffer;
    char* end = p + sizeof(buffer);

    constexpr AVal name = {const_cast<char*>(onMetaData_str.data()), (int)onMetaData_str.size()};
    p = AMF_EncodeString(p, end, &name);

    *p++ = AMF_ECMA_ARRAY;
    p = AMF_EncodeInt32(p, end, elem_count);

    enc_num_val(p, end, "duration", 0.0);
    enc_num_val(p, end, "fileSize", 0.0);
    
    UINT32 frame_width, frame_height;
    CHECK_HR(hr = MFGetAttributeSize(this->video_type, MF_MT_FRAME_SIZE, &frame_width, &frame_height));
    enc_num_val(p, end, "width", frame_width);
    enc_num_val(p, end, "height", frame_height);

    enc_str_val(p, end, "videocodecid", "avc1");

    UINT32 avg_bitrate;
    CHECK_HR(hr = this->video_type->GetUINT32(MF_MT_AVG_BITRATE, &avg_bitrate));
    enc_num_val(p, end, "videodatarate", avg_bitrate / 1000);

    UINT32 fps_num, fps_den;
    CHECK_HR(hr = MFGetAttributeRatio(this->video_type, MF_MT_FRAME_RATE, &fps_num, &fps_den));
    enc_num_val(p, end, "framerate", (double)fps_num / fps_den);

    enc_str_val(p, end, "audiocodecid", "mp4a");

    CHECK_HR(hr = this->audio_type->GetUINT32(MF_MT_AVG_BITRATE, &avg_bitrate));
    enc_num_val(p, end, "audiodatarate", avg_bitrate / 1000);

    UINT32 sample_rate;
    CHECK_HR(hr = this->audio_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate));
    enc_num_val(p, end, "audiosamplerate", sample_rate);

    UINT32 audio_bits_per_sample;
    CHECK_HR(hr = this->audio_type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &audio_bits_per_sample));
    enc_num_val(p, end, "audiosamplesize", audio_bits_per_sample);

    UINT32 audio_num_channels;
    CHECK_HR(hr = this->audio_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &audio_num_channels));
    enc_num_val(p, end, "audiochannels", audio_num_channels);

    enc_bool_val(p, end, "stereo", audio_num_channels == 2);
    enc_bool_val(p, end, "2.1", audio_num_channels == 3);
    enc_bool_val(p, end, "3.1", audio_num_channels == 4);
    enc_bool_val(p, end, "4.0", audio_num_channels == 4);
    enc_bool_val(p, end, "4.1", audio_num_channels == 5);
    enc_bool_val(p, end, "5.1", audio_num_channels == 6);
    enc_bool_val(p, end, "7.1", audio_num_channels == 8);

    enc_str_val(p, end, "encoder", encoder);

    assert_((p + 2) < end);

    *p++ = 0;
    *p++ = 0;
    *p++ = AMF_OBJECT_END;

    // write to stream
    {
        const uint32_t metadata_len = (uint32_t)(p - buffer);
        const uint32_t rtmp_body_size = sizeof(flv_tag) + metadata_len;
        const uint32_t flv_data_size = rtmp_body_size - sizeof(flv_tag);

        std::string packet(rtmp_body_size + 4, '\0');

        flv_tag* tag = (flv_tag*)packet.data();
        tag->tag_type = RTMP_PACKET_TYPE_INFO;
        tag->data_size[2] = (uint8_t)flv_data_size;
        tag->data_size[1] = (uint8_t)(flv_data_size >> 8);
        tag->data_size[0] = (uint8_t)(flv_data_size >> 16);
        tag->timestamp[2] = 0;
        tag->timestamp[1] = 0;
        tag->timestamp[0] = 0;
        tag->timestamp_extended = 0;
        tag->stream_id[2] = 0;
        tag->stream_id[1] = 0;
        tag->stream_id[0] = 0;

        assert_(sizeof(flv_tag) + metadata_len == rtmp_body_size);
        memcpy(
            packet.data() + sizeof(flv_tag),
            buffer,
            metadata_len);

        *(uint32_t*)(packet.data() + rtmp_body_size) = _byteswap_ulong(rtmp_body_size - 0);

        const int res = RTMP_Write(this->rtmp, packet.data(), (int)packet.size(), 0);
        if(!res)
            CHECK_HR(hr = E_UNEXPECTED);
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void output_rtmp::initialize(
    const std::string_view& url,
    const std::string_view& streaming_key,
    CWindow recording_initiator,
    const CComPtr<IMFMediaType>& video_type,
    const CComPtr<IMFMediaType>& audio_type)
{
    assert_(!this->rtmp);
    assert_(video_type);
    assert_(audio_type);

    HRESULT hr = S_OK;
    this->video_type = video_type;
    this->audio_type = audio_type;
    this->recording_initiator = recording_initiator;
    this->rtmp = RTMP_Alloc();
    if(!this->rtmp)
        CHECK_HR(hr = E_UNEXPECTED);

    // The stream should contain raw_data_block elements only
    UINT32 aac_payload_type = MFGetAttributeUINT32(this->audio_type, MF_MT_AAC_PAYLOAD_TYPE, 0);
    if(aac_payload_type != 0)
        CHECK_HR(hr = E_UNEXPECTED);

    RTMP_Init(this->rtmp);

    RTMP_LogSetLevel(RTMP_LOGALL);
    RTMP_LogSetCallback(
        [](int level, const char* fmt, va_list args) 
        {
            if(level == RTMP_LOGCRIT || level == RTMP_LOGERROR || level == RTMP_LOGWARNING)
            {
                vprintf(fmt, args); 
                printf("\n");
            }
            /*vprintf(fmt, args); printf("\n");*/ 
        });

    // Additional options may be specified by appending space-separated key=value pairs to the URL.
    /*str += " live=1";*/

    if(!RTMP_SetupURL(this->rtmp, const_cast<char*>(url.data())))
        CHECK_HR(hr = E_UNEXPECTED);
    RTMP_EnableWrite(this->rtmp);

    {
        static constexpr std::string_view str = "FMLE/3.0 (compatible; FMSc/1.0)";
        AVal val = {const_cast<char*>(str.data()), (int)str.size()};
        this->rtmp->Link.flashVer = val;
    }
    this->rtmp->Link.swfUrl = this->rtmp->Link.tcUrl;

    RTMP_AddStream(this->rtmp, streaming_key.data());
    
    this->rtmp->m_outChunkSize = 4096;
    this->rtmp->m_bSendChunkSizeInfo = true;
    this->rtmp->m_bUseNagle = true;

    if(!RTMP_Connect(this->rtmp, nullptr))
        CHECK_HR(hr = E_UNEXPECTED);
    if(!RTMP_ConnectStream(this->rtmp, 0))
        CHECK_HR(hr = E_UNEXPECTED);

    this->send_flv_metadata();

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

// potentially uninitialized local variable (samplingFrequencyIndex, channelConfiguration)
#pragma warning(push)
#pragma warning(disable: 4701)
std::string output_rtmp::create_audio_specific_config() const
{
    HRESULT hr = S_OK;

    // audio specific config size: 16 bits
    uint16_t audioObjectType;           // 5 bits
    uint16_t samplingFrequencyIndex;    // 4 bits
    uint16_t channelConfiguration;      // 4 bits
    uint16_t padding;                   // 3 bits

    UINT32 samples_per_second, audio_num_channels;

    audioObjectType = 2; // AAC LC

    CHECK_HR(hr = this->audio_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &samples_per_second));

    if(samples_per_second == 44100)
        samplingFrequencyIndex = 0x4; // 44100
    else if(samples_per_second == 48000)
        samplingFrequencyIndex = 0x3; // 48000
    else
        CHECK_HR(hr = E_UNEXPECTED);
    
    CHECK_HR(hr = this->audio_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &audio_num_channels));

    if(audio_num_channels == 2)
        channelConfiguration = 2;
    else
        CHECK_HR(hr = E_UNEXPECTED);

    padding = 0;

    uint16_t audio_specific_config = 0;
    audio_specific_config |= audioObjectType        << (16 - 5);
    audio_specific_config |= samplingFrequencyIndex << (16 - 5 - 4);
    audio_specific_config |= channelConfiguration   << (16 - 5 - 4 - 4);
    audio_specific_config |= padding                << (16 - 5 - 4 - 4 - 3);

    audio_specific_config = _byteswap_ushort(audio_specific_config);

    {
        std::string config_str((const char*)&audio_specific_config, sizeof(audio_specific_config));
        return config_str;
    }

done:
    throw HR_EXCEPTION(hr);
}
#pragma warning(pop)

void output_rtmp::send_rtmp_audio_packets(const std::string_view& data, LONGLONG ts)
{
    if(ts < 0)
        throw HR_EXCEPTION(E_UNEXPECTED);

    const uint32_t timestamp_ms = (uint32_t)((double)ts / SECOND_IN_TIME_UNIT * 1000.0);

#pragma pack(push, 1)
    // ms specific: the ordering of data declared as bit fields is from low to high bit
    struct flv_audio_tag
    {
        struct
        {
            uint8_t sound_type : 1;
            uint8_t sound_size : 1;
            uint8_t sound_rate : 2;
            uint8_t sound_format : 4;
        };

        struct aac_audio_data_t
        {
            uint8_t aac_packet_type;
            // unsigned char data[]
        } aac_audio_data;
    };
#pragma pack(pop)

    if(!this->audio_headers_sent)
    {
        this->audio_headers_sent = true;

        std::string audio_specific_config = this->create_audio_specific_config();
        const uint32_t rtmp_body_size = 
            sizeof(flv_tag) + sizeof(flv_audio_tag) + (uint32_t)audio_specific_config.size();
        const uint32_t flv_data_size = rtmp_body_size - sizeof(flv_tag);

        std::string packet(rtmp_body_size + 4, '\0');

        flv_tag* tag = (flv_tag*)packet.data();
        tag->tag_type = RTMP_PACKET_TYPE_AUDIO;
        tag->data_size[2] = (uint8_t)flv_data_size;
        tag->data_size[1] = (uint8_t)(flv_data_size >> 8);
        tag->data_size[0] = (uint8_t)(flv_data_size >> 16);
        tag->timestamp[2] = (uint8_t)timestamp_ms;
        tag->timestamp[1] = (uint8_t)(timestamp_ms >> 8);
        tag->timestamp[0] = (uint8_t)(timestamp_ms >> 16);
        tag->timestamp_extended = (uint8_t)(timestamp_ms >> 24);
        tag->stream_id[2] = 0;
        tag->stream_id[1] = 0;
        tag->stream_id[0] = 0;

        flv_audio_tag* audio_tag = (flv_audio_tag*)(packet.data() + sizeof(flv_tag));
        audio_tag->sound_format = 10; // aac
        audio_tag->sound_rate = 3; // for aac always 3
        audio_tag->sound_size = 1; // only pertains to uncompressed formats
        audio_tag->sound_type = 1; // for aac always 1
        audio_tag->aac_audio_data.aac_packet_type = 0; // aac sequence header

        assert_(
            sizeof(flv_tag) + sizeof(flv_audio_tag) + audio_specific_config.size() == rtmp_body_size);
        memcpy(
            packet.data() + sizeof(flv_tag) + sizeof(flv_audio_tag),
            audio_specific_config.data(),
            audio_specific_config.size());

        *(uint32_t*)(packet.data() + rtmp_body_size) = _byteswap_ulong(rtmp_body_size - 0);

        const int res = RTMP_Write(this->rtmp, packet.data(), (int)packet.size(), 0);
        if(!res)
            throw HR_EXCEPTION(E_UNEXPECTED);
    }

    const uint32_t rtmp_body_size = 
        sizeof(flv_tag) + sizeof(flv_audio_tag) + (uint32_t)data.size();
    const uint32_t flv_data_size = rtmp_body_size - sizeof(flv_tag);

    std::string packet(rtmp_body_size + 4, '\0');

    flv_tag* tag = (flv_tag*)packet.data();
    tag->tag_type = RTMP_PACKET_TYPE_AUDIO;
    tag->data_size[2] = (uint8_t)flv_data_size;
    tag->data_size[1] = (uint8_t)(flv_data_size >> 8);
    tag->data_size[0] = (uint8_t)(flv_data_size >> 16);
    tag->timestamp[2] = (uint8_t)timestamp_ms;
    tag->timestamp[1] = (uint8_t)(timestamp_ms >> 8);
    tag->timestamp[0] = (uint8_t)(timestamp_ms >> 16);
    tag->timestamp_extended = (uint8_t)(timestamp_ms >> 24);
    tag->stream_id[2] = 0;
    tag->stream_id[1] = 0;
    tag->stream_id[0] = 0;

    flv_audio_tag* audio_tag = (flv_audio_tag*)(packet.data() + sizeof(flv_tag));
    audio_tag->sound_format = 10;
    audio_tag->sound_rate = 3;
    audio_tag->sound_size = 0;
    audio_tag->sound_type = 1;
    audio_tag->aac_audio_data.aac_packet_type = 1; // raw aac frame data

    assert_(sizeof(flv_tag) + sizeof(flv_audio_tag) + data.size() == rtmp_body_size);
    memcpy(
        packet.data() + sizeof(flv_tag) + sizeof(flv_audio_tag),
        data.data(),
        data.size());

    *(uint32_t*)(packet.data() + rtmp_body_size) = _byteswap_ulong(rtmp_body_size - 0);

    const int res = RTMP_Write(this->rtmp, packet.data(), (int)packet.size(), 0);
    if(!res)
        throw HR_EXCEPTION(E_UNEXPECTED);
}

std::string output_rtmp::create_avc_decoder_configuration_record(
    const std::string_view& sps_nalu, const std::string_view& pps_nalu,
    int start_code_prefix_len) const
{
    assert_(start_code_prefix_len > 0 && start_code_prefix_len <= 4);

    HRESULT hr = S_OK;

    if(sps_nalu.size() > std::numeric_limits<uint16_t>::max() ||
        pps_nalu.size() > std::numeric_limits<uint16_t>::max())
        throw HR_EXCEPTION(E_UNEXPECTED);

    // http://www.staroceans.org/e-book/ISO-14496-10.pdf

    // ISO/IEC 14496-15:2004(E)
    // http://nhbzh.com/asp/admin/editor/newsfile/201011314552121.pdf
#pragma pack(push, 1)
    // in big endian format;
    // ms specific: the ordering of data declared as bit fields is from low to high bit
    struct AVCDecoderConfigurationRecord
    {
        uint8_t configurationVersion;
        uint8_t AVCProfileIndication;
        uint8_t profile_compatibility;
        uint8_t AVCLevelIndication;

        struct
        {
            uint8_t lengthSizeMinusOne : 2;
            uint8_t reserved : 6; // must be filled with 1's
        };
        struct
        {
            uint8_t numOfSequenceParameterSets : 5; // 1 for now
            uint8_t reserved2 : 3; // must be filled with 1's
        };

        //struct
        //{
        //    uint8_t reserved : 6; // must be filled with 1's
        //    uint8_t lengthSizeMinusOne : 2;
        //};
        //struct
        //{
        //    uint8_t reserved2 : 3; // must be filled with 1's
        //    uint8_t numOfSequenceParameterSets : 5; // 1 for now
        //};

        uint16_t sequenceParameterSetLength;
        // for (i=0; i< numOfSequenceParameterSets; i++) { 
        //    uint(8*sequenceParameterSetLength) sequenceParameterSetNALUnit
        // }
        // uint8_t numOfPictureParameterSets; // 1
        // for(i = 0; i < numOfPictureParameterSets; i++) {
        //    uint16_t pictureParameterSetLength;
        //    uint(8*pictureParameterSetLength) pictureParameterSetNALUnit;
        // }
    };
#pragma pack(pop)

    std::string record_str(
        sizeof(AVCDecoderConfigurationRecord) + sps_nalu.size() +
        sizeof(uint8_t) + sizeof(uint16_t) + pps_nalu.size(), '\0');

    AVCDecoderConfigurationRecord* record = (AVCDecoderConfigurationRecord*)record_str.data();
    UINT32 profile_indication, level_indication;

    record->configurationVersion = 1;

    CHECK_HR(hr = this->video_type->GetUINT32(MF_MT_MPEG2_PROFILE, &profile_indication));
    if(profile_indication > std::numeric_limits<uint8_t>::max())
        CHECK_HR(hr = E_UNEXPECTED);

    // TODO: profile_indication(=profile_idc) is located at sps_nalu.at(1)

    record->AVCProfileIndication = (uint8_t)profile_indication;
    record->profile_compatibility = sps_nalu.at(2);

    CHECK_HR(hr = this->video_type->GetUINT32(MF_MT_MPEG2_LEVEL, &level_indication));
    if(level_indication > std::numeric_limits<uint8_t>::max())
        CHECK_HR(hr = E_UNEXPECTED);

    record->AVCLevelIndication = (uint8_t)level_indication;
    record->reserved = ~(record->reserved & 0);
    record->lengthSizeMinusOne = (uint8_t)(start_code_prefix_len - 1);
    record->reserved2 = ~(record->reserved2 & 0);
    record->numOfSequenceParameterSets = 1;
    record->sequenceParameterSetLength = _byteswap_ushort((uint16_t)sps_nalu.size());

    char* p = (char*)memcpy(
        record_str.data() + sizeof(AVCDecoderConfigurationRecord),
        sps_nalu.data(),
        sps_nalu.size());

    p += sps_nalu.size();
    *p++ = 1; // numOfPictureParameterSets
    *(uint16_t*)p = _byteswap_ushort((uint16_t)pps_nalu.size());
    p += sizeof(uint16_t);
    
    memcpy(p, pps_nalu.data(), pps_nalu.size());

    assert_(p + pps_nalu.size() == record_str.data() + record_str.size());

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);

    return record_str;
}

// from ffmpeg(LGPL)
const uint8_t* ff_avc_find_startcode_internal(const uint8_t* p,
    const uint8_t* end)
{
    const uint8_t* a = p + 4 - ((intptr_t)p & 3);

    for(end -= 3; p < a && p < end; p++) {
        if(p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    for(end -= 3; p < end; p += 4) {
        uint32_t x = *(const uint32_t*)p;

        if((x - 0x01010101) & (~x) & 0x80808080) {
            if(p[1] == 0) {
                if(p[0] == 0 && p[2] == 1)
                    return p;
                if(p[2] == 0 && p[3] == 1)
                    return p + 1;
            }

            if(p[3] == 0) {
                if(p[2] == 0 && p[4] == 1)
                    return p + 2;
                if(p[4] == 0 && p[5] == 1)
                    return p + 3;
            }
        }
    }

    for(end += 3; p < end; p++) {
        if(p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    return end + 3;
}

std::size_t output_rtmp::find_start_code_prefix(
    const std::string_view& data, int& start_code_prefix_len)
{
    start_code_prefix_len = 4;

    const uint8_t* end = ff_avc_find_startcode_internal(
        (uint8_t*)data.data(),
        (uint8_t*)data.data() + data.size());

    if(end >= (const uint8_t*)data.data() + data.size())
        return std::string_view::npos;
    else
        return end - (uint8_t*)data.data();

    /*std::size_t pos = data.find("\0\0", 0, 2);

    for(; pos != std::string_view::npos; pos = data.find("\0\0", pos + 2, 2))
    {
        const char c = data.at(pos + 2);
        if(c == 0x01)
        {
            start_code_prefix_len = 3;
            break;
        }
        else if(c == 0x00 && data.at(pos + 3) == 0x01)
        {
            start_code_prefix_len = 4;
            break;
        }
    }

    return pos;*/
}

void output_rtmp::send_rtmp_video_packets(
    const std::string_view& data, LONGLONG pts, LONGLONG dts, bool key_frame)
{
    if(pts < 0 || dts < 0)
        throw HR_EXCEPTION(E_UNEXPECTED);

    const uint32_t timestamp_ms = (uint32_t)((double)pts / SECOND_IN_TIME_UNIT * 1000.0);

    // nalu start code prefix is either 00 00 00 01 or 00 00 01;
    // these are unique in the data
    int start_code_prefix_len;
    std::size_t nalu_start = find_start_code_prefix(data, start_code_prefix_len);
    if(nalu_start == std::string_view::npos)
        throw HR_EXCEPTION(E_UNEXPECTED);

    // https://www.adobe.com/content/dam/acom/en/devnet/flv/video_file_format_spec_v10_1.pdf
#pragma pack(push, 1)
    // big endian
    // ms specific: the ordering of data declared as bit fields is from low to high bit;
    // evidently the rule above doesn't apply in every situation;
    // bits in avc_video_packet aren't swapped
    struct flv_video_tag
    {
        struct
        {
            uint8_t codec_id : 4;
            uint8_t frame_type : 4;
        };

        struct avc_video_packet_t
        {
            uint32_t avc_packet_type : 8;
            uint32_t composition_time : 24; // composition time offset in milliseconds
            // unsigned char data[]
        } avc_video_packet;
    };
#pragma pack(pop)

    // TODO: padding nalus could be used to stabilize the output bitrate

    std::string payload;

    std::string_view data_chunk = data;
    while(nalu_start != std::string_view::npos)
    {
        data_chunk = data_chunk.substr(nalu_start);
        for(int i = 0; !data_chunk.at(0); i++)
            data_chunk = data_chunk.substr(i + 1);

        int next_start_code_prefix_len;
        const std::size_t next_nalu_start = 
            find_start_code_prefix(data_chunk, next_start_code_prefix_len);

        const std::string_view nalu = data_chunk.substr(0, next_nalu_start - 1);
        const unsigned char nalu_header = nalu.at(0);

        // check that the forbidden zero isn't set
        if(nalu_header & 0x80)
            throw HR_EXCEPTION(E_UNEXPECTED);

        // 2 bits
        const unsigned char nalu_ref_idc = nalu_header >> 5; nalu_ref_idc;
        // 5 bits
        const unsigned char nalu_type = nalu_header & ~(0x80 | 0x40 | 0x20);

        // store the sps and pps if no headers are sent yet
        if(!this->video_headers_sent)
        {
            if(nalu_type == 7)
                this->sps_nalu = nalu;
            else if(nalu_type == 8)
                this->pps_nalu = nalu;
        }

        if(!this->video_headers_sent && !this->sps_nalu.empty() && !this->pps_nalu.empty())
        {
            this->video_headers_sent = true;

            std::string avc_decoder_configuration_record =
                this->create_avc_decoder_configuration_record(
                    this->sps_nalu, this->pps_nalu, start_code_prefix_len);
            const uint32_t rtmp_body_size = sizeof(flv_tag) + sizeof(flv_video_tag) +
                (uint32_t)avc_decoder_configuration_record.size();
            const uint32_t flv_data_size = rtmp_body_size - sizeof(flv_tag);

            std::string packet(rtmp_body_size + 4, '\0');

            flv_tag* tag = (flv_tag*)packet.data();
            tag->tag_type = RTMP_PACKET_TYPE_VIDEO;
            tag->data_size[2] = (uint8_t)flv_data_size;
            tag->data_size[1] = (uint8_t)(flv_data_size >> 8);
            tag->data_size[0] = (uint8_t)(flv_data_size >> 16);
            tag->timestamp[2] = (uint8_t)timestamp_ms;
            tag->timestamp[1] = (uint8_t)(timestamp_ms >> 8);
            tag->timestamp[0] = (uint8_t)(timestamp_ms >> 16);
            tag->timestamp_extended = (uint8_t)(timestamp_ms >> 24);
            tag->stream_id[2] = 0;
            tag->stream_id[1] = 0;
            tag->stream_id[0] = 0;

            flv_video_tag* video_tag = (flv_video_tag*)(packet.data() + sizeof(flv_tag));
            video_tag->frame_type = key_frame ? 1 : 2;
            video_tag->codec_id = 7;

            video_tag->avc_video_packet.avc_packet_type = 0;
            video_tag->avc_video_packet.composition_time = 0;

            assert_(
                sizeof(flv_tag) + sizeof(flv_video_tag) + avc_decoder_configuration_record.size() ==
                rtmp_body_size);
            memcpy(
                packet.data() + sizeof(flv_tag) + sizeof(flv_video_tag),
                avc_decoder_configuration_record.data(),
                avc_decoder_configuration_record.size());

            *(uint32_t*)(packet.data() + rtmp_body_size) = _byteswap_ulong(rtmp_body_size - 0);

            const int res = RTMP_Write(this->rtmp, packet.data(), (int)packet.size(), 0);
            if(!res)
                throw HR_EXCEPTION(E_UNEXPECTED);
        }

        if(nalu_type <= 5 || nalu_type == 6)
        {
            const uint32_t size = (uint32_t)payload.size();
            const uint32_t nalu_size = _byteswap_ulong((uint32_t)nalu.size());

            payload.append((const char*)&nalu_size, sizeof(uint32_t));
            payload += nalu;
        }

        nalu_start = next_nalu_start;
    }

    // send the payload
    const uint32_t rtmp_body_size =
        sizeof(flv_tag) + sizeof(flv_video_tag) + (uint32_t)payload.size();
    const uint32_t flv_data_size = rtmp_body_size - sizeof(flv_tag);

    std::string packet(rtmp_body_size + 4, '\0');

    flv_tag* tag = (flv_tag*)packet.data();
    tag->tag_type = RTMP_PACKET_TYPE_VIDEO;
    tag->data_size[2] = (uint8_t)flv_data_size;
    tag->data_size[1] = (uint8_t)(flv_data_size >> 8);
    tag->data_size[0] = (uint8_t)(flv_data_size >> 16);
    tag->timestamp[2] = (uint8_t)timestamp_ms;
    tag->timestamp[1] = (uint8_t)(timestamp_ms >> 8);
    tag->timestamp[0] = (uint8_t)(timestamp_ms >> 16);
    tag->timestamp_extended = (uint8_t)(timestamp_ms >> 24);
    tag->stream_id[2] = 0;
    tag->stream_id[1] = 0;
    tag->stream_id[0] = 0;

    flv_video_tag* video_tag = (flv_video_tag*)(packet.data() + sizeof(flv_tag));
    video_tag->frame_type = key_frame ? 1 : 2;
    video_tag->codec_id = 7;

    video_tag->avc_video_packet.avc_packet_type = 1;
    int32_t composition_time = (int32_t)((double)(pts - dts) / SECOND_IN_TIME_UNIT * 1000.0);
    video_tag->avc_video_packet.composition_time = _byteswap_ulong(composition_time) >> 8;

    assert_(sizeof(flv_tag) + sizeof(flv_video_tag) + (uint32_t)payload.size() == rtmp_body_size);
    memcpy(
        packet.data() + sizeof(flv_tag) + sizeof(flv_video_tag),
        payload.data(),
        payload.size());

    *(uint32_t*)(packet.data() + rtmp_body_size) = _byteswap_ulong(rtmp_body_size - 0);

    const int res = RTMP_Write(this->rtmp, packet.data(), (int)packet.size(), 0);
    if(!res)
        throw HR_EXCEPTION(E_UNEXPECTED);
}

void output_rtmp::send_rtmp_packets()
{
    HRESULT hr = S_OK;
    LONGLONG video_ts, audio_ts;
    LONGLONG video_dur, audio_dur;

    CComPtr<IMFMediaBuffer> media_buffer;
    BYTE* buffer = nullptr;

    while(!this->video_samples.empty() && !this->audio_samples.empty())
    {
        DWORD buffer_len;

        auto&& video_sample = this->video_samples[0];
        auto&& audio_sample = this->audio_samples[0];

        CHECK_HR(hr = video_sample->GetSampleTime(&video_ts));
        CHECK_HR(hr = video_sample->GetSampleDuration(&video_dur));
        CHECK_HR(hr = audio_sample->GetSampleTime(&audio_ts));
        CHECK_HR(hr = audio_sample->GetSampleDuration(&audio_dur));

        auto& selected_sample = (video_ts <= audio_ts) ? video_sample : audio_sample;

        CHECK_HR(hr = selected_sample->GetBufferByIndex(0, &media_buffer));
        CHECK_HR(hr = media_buffer->GetCurrentLength(&buffer_len));
        if(buffer_len == 0)
            CHECK_HR(hr = E_UNEXPECTED);
        CHECK_HR(hr = media_buffer->Lock(&buffer, nullptr, nullptr));

        if(video_ts <= audio_ts)
        {
            const std::string_view data((char*)buffer, buffer_len);
            UINT32 key_frame;
            LONGLONG dts;

            CHECK_HR(hr = video_sample->GetUINT32(MFSampleExtension_CleanPoint, &key_frame));
            dts = MFGetAttributeUINT64(
                video_sample, MFSampleExtension_DecodeTimestamp, video_ts);

            try
            {
                this->send_rtmp_video_packets(data, video_ts, dts, (bool)key_frame);
            }
            catch(streaming::exception err)
            {
                CHECK_HR(hr = err.get_hresult());
            }
            catch(std::exception)
            {
                CHECK_HR(hr = E_UNEXPECTED);
            }

            this->video_samples.pop_front();
        }
        else
        {
            const std::string_view data((char*)buffer, buffer_len);

            try
            {
                this->send_rtmp_audio_packets(data, audio_ts);
            }
            catch(streaming::exception err)
            {
                CHECK_HR(hr = err.get_hresult());
            }
            catch(std::exception)
            {
                CHECK_HR(hr = E_UNEXPECTED);
            }

            this->audio_samples.pop_front();
        }

        media_buffer->Unlock();
        buffer = nullptr;
        media_buffer = nullptr;
    }

done:
    if(media_buffer && buffer)
        media_buffer->Unlock();

    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void output_rtmp::write_sample(bool video, const CComPtr<IMFSample>& sample)
{
    assert_(sample);

    // rtmp most likely isn't multithread safe;
    // output_rtmp itself isn't multithread safe either
    scoped_lock lock(this->write_lock);

    if(video)
        this->video_samples.push_back(sample);
    else
        this->audio_samples.push_back(sample);

    this->send_rtmp_packets();
}