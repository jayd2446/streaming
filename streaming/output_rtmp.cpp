#include "output_rtmp.h"
#include <librtmp/log.h>
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

    this->recording_initiator.SendNotifyMessageW(RECORDING_STOPPED_MESSAGE);
}

bool output_rtmp::send_flv_metadata()
{
    assert_(RTMP_IsConnected(this->rtmp));

#pragma pack(push, 1)
    struct flv_header
    {
        uint8_t signature_f;
        uint8_t signature_l;
        uint8_t signature_v;
        uint8_t version;
        uint8_t flags;
        uint32_t data_offset;
    };
#pragma pack(pop)

    const uint32_t rtmp_body_size = sizeof(flv_header);
}

void output_rtmp::initialize(
    const char* url,
    CWindow recording_initiator,
    const CComPtr<IMFMediaType>& video_type,
    const CComPtr<IMFMediaType>& audio_type)
{
    assert_(!this->rtmp);
    assert_(video_type);
    assert_(audio_type);

    HRESULT hr = S_OK;
    std::string str = url;

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
    str += " live=1";

    if(!RTMP_SetupURL(this->rtmp, str.data()))
        CHECK_HR(hr = E_UNEXPECTED);
    RTMP_EnableWrite(this->rtmp);
    if(!RTMP_Connect(this->rtmp, nullptr))
        CHECK_HR(hr = E_UNEXPECTED);
    if(!RTMP_ConnectStream(this->rtmp, 0))
        CHECK_HR(hr = E_UNEXPECTED);

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

std::string output_rtmp::create_audio_specific_config() const
{
    HRESULT hr = S_OK;

#pragma pack(push, 1)
    // in big endian format;
    // ms specific: the ordering of data declared as bit fields is from low to high bit
    struct AudioSpecificConfig
    {
        /*uint8_t audioObjectType : 5;
        uint8_t samplingFrequencyIndex : 4;
        uint32_t samplingFrequency : 24;
        uint8_t channelConfiguration : 4;
        uint8_t epConfig : 2;
        bool directMapping : 1;*/
    };
#pragma pack(pop)

    // audio specific config size: 40 bits
    uint64_t audioObjectType;           // 5 bits
    uint64_t samplingFrequencyIndex;    // 4 bits
    uint64_t samplingFrequency;         // 24 bits
    uint64_t channelConfiguration;      // 4 bits
    uint64_t epConfig;                  // 2 bits
    uint64_t directMapping;             // 1 bit

    /*std::string config_str(sizeof(AudioSpecificConfig), '\0');*/
    UINT32 samples_per_second, audio_num_channels;

    /*AudioSpecificConfig* config = (AudioSpecificConfig*)config_str.data();*/
    audioObjectType = 2; // AAC LC

    CHECK_HR(hr = this->audio_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &samples_per_second));

    if(samples_per_second == 44100)
        samplingFrequencyIndex = 0x4; // 44100
    else if(samples_per_second == 48000)
        samplingFrequencyIndex = 0x3; // 48000
    else
        CHECK_HR(hr = E_UNEXPECTED);

    samplingFrequency = 0;
    
    CHECK_HR(hr = this->audio_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &audio_num_channels));

    if(audio_num_channels == 2)
        channelConfiguration = 2;
    else
        CHECK_HR(hr = E_UNEXPECTED);

    epConfig = 0;       // not used
    directMapping = 0;  // not used

    uint64_t audio_specific_config = 0;
    audio_specific_config |= audioObjectType        << (40 - 5);
    audio_specific_config |= samplingFrequencyIndex << (40 - 5 - 4);
    audio_specific_config |= samplingFrequency      << (40 - 5 - 4 - 24);
    audio_specific_config |= channelConfiguration   << (40 - 5 - 4 - 24 - 4);
    audio_specific_config |= epConfig               << (40 - 5 - 4 - 24 - 4 - 2);
    audio_specific_config |= directMapping          << (40 - 5 - 4 - 24 - 4 - 2 - 1);

    audio_specific_config = _byteswap_uint64(audio_specific_config);

    {
        std::string config_str((const char*)&audio_specific_config + 24 / 8, 40 / 8);
        return config_str;
    }

done:
    throw HR_EXCEPTION(hr);
}

void output_rtmp::send_rtmp_audio_packets(const std::string_view& data, LONGLONG ts)
{
    if(ts < 0)
        throw HR_EXCEPTION(E_UNEXPECTED);

    const uint32_t timestamp_ms = RTMP_GetTime();//(uint32_t)((double)ts / SECOND_IN_TIME_UNIT * 1000.0);

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
        audio_tag->sound_size = 0; // only pertains to uncompressed formats
        audio_tag->sound_type = 1; // for aac always 1
        audio_tag->aac_audio_data.aac_packet_type = 0; // aac sequence header

        assert_(
            sizeof(flv_tag) + sizeof(flv_audio_tag) + audio_specific_config.size() == rtmp_body_size);
        memcpy(
            packet.data() + sizeof(flv_tag) + sizeof(flv_audio_tag),
            audio_specific_config.data(),
            audio_specific_config.size());

        *(uint32_t*)(packet.data() + packet.size() - 4) = _byteswap_ulong(rtmp_body_size - 1);

        const int res = RTMP_Write(this->rtmp, packet.data(), (int)packet.size());
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

    *(uint32_t*)(packet.data() + packet.size() - 4) = _byteswap_ulong(rtmp_body_size - 1);

    const int res = RTMP_Write(this->rtmp, packet.data(), (int)packet.size());
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

    uint8_t* p = (uint8_t*)memcpy(
        record_str.data() + sizeof(AVCDecoderConfigurationRecord),
        sps_nalu.data(),
        sps_nalu.size());

    p += sps_nalu.size();
    *p++ = 1; // numOfPictureParameterSets
    *(uint16_t*)p = _byteswap_ushort((uint16_t)pps_nalu.size());
    p += sizeof(uint16_t);
    
    memcpy(p, pps_nalu.data(), pps_nalu.size());

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);

    return record_str;
}

std::size_t output_rtmp::find_start_code_prefix(
    const std::string_view& data, int& start_code_prefix_len)
{
    std::size_t pos = data.find("\0\0", 0, 2);

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

    return pos;
}

void output_rtmp::send_rtmp_video_packets(
    const std::string_view& data, LONGLONG pts, LONGLONG dts, bool key_frame)
{
    if(pts < 0 || dts < 0)
        throw HR_EXCEPTION(E_UNEXPECTED);
    assert_(pts >= dts);

    const uint32_t timestamp_ms = RTMP_GetTime();//(uint32_t)((double)pts / SECOND_IN_TIME_UNIT * 1000.0);

    // nalu start code prefix is either 00 00 00 01 or 00 00 01;
    // these are unique in the data
    int start_code_prefix_len;
    std::size_t start_code_prefix_pos = find_start_code_prefix(data, start_code_prefix_len);
    if(start_code_prefix_pos != 0)
        std::cout << "warning: h264 data stream contains unexpected data" << std::endl;
    if(start_code_prefix_pos == std::string_view::npos)
        throw HR_EXCEPTION(E_UNEXPECTED);

    // https://www.adobe.com/content/dam/acom/en/devnet/flv/video_file_format_spec_v10.pdf
#pragma pack(push, 1)
        // big endian
        // ms specific: the ordering of data declared as bit fields is from low to high bit
    struct flv_video_tag
    {
        struct
        {
            uint8_t codec_id : 4;
            uint8_t frame_type : 4;
        };

        /*struct
        {
            uint8_t frame_type : 4;
            uint8_t codec_id : 4;
        };*/
        struct avc_video_packet_t
        {
            // avc_packet_type: 8 bits
            // composition time: 24 bits
            int32_t avc_packet_type_and_composition_time;
            //uint8_t avc_packet_type;
            //int32_t composition_time : 24; // composition time offset in milliseconds
            // unsigned char data[]
        } avc_video_packet;
    };
#pragma pack(pop)

    std::string_view data_chunk = data;
    while(start_code_prefix_pos != std::string_view::npos)
    {
        data_chunk = data_chunk.substr(start_code_prefix_pos + start_code_prefix_len);

        int next_start_code_prefix_len;
        const std::size_t next_start_code_prefix_pos = 
            find_start_code_prefix(data_chunk, next_start_code_prefix_len);

        if(next_start_code_prefix_pos != std::string_view::npos)
            if(next_start_code_prefix_len != start_code_prefix_len)
                throw HR_EXCEPTION(E_UNEXPECTED);

        const std::string_view nalu = data_chunk.substr(0, next_start_code_prefix_pos);
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

            constexpr uint32_t avc_packet_type = 0, composition_time = 0;
            video_tag->avc_video_packet.avc_packet_type_and_composition_time =
                _byteswap_ulong((avc_packet_type << 24) | composition_time);

            assert_(
                sizeof(flv_tag) + sizeof(flv_video_tag) + avc_decoder_configuration_record.size() ==
                rtmp_body_size);
            memcpy(
                packet.data() + sizeof(flv_tag) + sizeof(flv_video_tag),
                avc_decoder_configuration_record.data(),
                avc_decoder_configuration_record.size());

            *(uint32_t*)(packet.data() + packet.size() - 4) = _byteswap_ulong(rtmp_body_size - 1);

            const int res = RTMP_Write(this->rtmp, packet.data(), (int)packet.size());
            /*const int res = RTMP_SendPacket(this->rtmp, &packet, 0);
            RTMPPacket_Free(&packet);*/
            if(!res)
                throw HR_EXCEPTION(E_UNEXPECTED);
        }

        /*const uint32_t rtmp_body_size = 
            sizeof(flv_tag) + sizeof(flv_video_tag) + (uint32_t)nalu.size();
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

        const uint32_t avc_packet_type = 1,
            composition_time = 
            (uint32_t)((double)(pts - dts) / SECOND_IN_TIME_UNIT * 1000.0) & 0x00ffffff;
        video_tag->avc_video_packet.avc_packet_type_and_composition_time =
            _byteswap_ulong((avc_packet_type << 24) | composition_time);

        assert_(sizeof(flv_tag) + sizeof(flv_video_tag) + (uint32_t)nalu.size() == rtmp_body_size);
        memcpy(
            packet.data() + sizeof(flv_tag) + sizeof(flv_video_tag),
            nalu.data(),
            nalu.size());

        *(uint32_t*)(packet.data() + packet.size() - 4) = _byteswap_ulong(rtmp_body_size - 1);

        const int res = RTMP_Write(this->rtmp, packet.data(), (int)packet.size());
        if(!res)
            throw HR_EXCEPTION(E_UNEXPECTED);*/

        //uint32_t rtmp_body_size;
        //std::string avc_decoder_configuration_record;

        //if(!this->video_headers_sent && !this->sps_nalu.empty() && !this->pps_nalu.empty())
        //    avc_decoder_configuration_record =
        //    this->create_avc_decoder_configuration_record(this->sps_nalu, this->pps_nalu,
        //        start_code_prefix_len);

        //if(this->video_headers_sent)
        //    rtmp_body_size = sizeof(flv_video_tag) + nalu_size;
        //else if(!avc_decoder_configuration_record.empty())
        //    rtmp_body_size = sizeof(flv_video_tag) + (uint32_t)avc_decoder_configuration_record.size();
        //else
        //    rtmp_body_size = 0;

        //if(rtmp_body_size)
        //{
        //    RTMPPacket packet = {0};
        //    if(!RTMPPacket_Alloc(&packet, rtmp_body_size))
        //        throw HR_EXCEPTION(E_UNEXPECTED);

        //    packet.m_packetType = RTMP_PACKET_TYPE_VIDEO;
        //    packet.m_hasAbsTimestamp = TRUE;
        //    packet.m_nChannel = 4;
        //    packet.m_nInfoField2 = this->rtmp->m_stream_id;
        //    packet.m_nBodySize = rtmp_body_size;
        //    packet.m_nTimeStamp = RTMP_GetTime();//(uint32_t)((double)pts / SECOND_IN_TIME_UNIT * 1000.0);

        //    flv_video_tag* video_tag = (flv_video_tag*)packet.m_body;
        //    video_tag->frame_type = key_frame ? 1 : 2;
        //    video_tag->codec_id = 7;

        //    if(!this->video_headers_sent)
        //    {
        //        packet.m_headerType = RTMP_PACKET_SIZE_MEDIUM;

        //        constexpr uint32_t avc_packet_type = 0,
        //            composition_time = 0;

        //        this->video_headers_sent = true;
        //        video_tag->avc_video_packet.avc_packet_type_and_composition_time =
        //            _byteswap_ulong((avc_packet_type << 24) | composition_time);

        //        assert_(sizeof(flv_video_tag) + avc_decoder_configuration_record.size() ==
        //            rtmp_body_size);
        //        memcpy(
        //            packet.m_body + sizeof(flv_video_tag),
        //            avc_decoder_configuration_record.data(),
        //            avc_decoder_configuration_record.size());

        //        const int res = RTMP_SendPacket(this->rtmp, &packet, 0);
        //    }
        //    /*else*/
        //    {
        //        // TODO: handle end of sequence, this should be sent when the stream is
        //        // ending

        //        packet.m_headerType = RTMP_PACKET_SIZE_LARGE;

        //        const uint32_t avc_packet_type = 1,
        //            composition_time = 
        //            (uint32_t)((double)(pts - dts) / SECOND_IN_TIME_UNIT * 1000.0) & 0x00ffffff;

        //        video_tag->avc_video_packet.avc_packet_type_and_composition_time =
        //            _byteswap_ulong((avc_packet_type << 24) | composition_time);

        //        assert_(sizeof(flv_video_tag) + nalu.size() == rtmp_body_size);
        //        memcpy(
        //            packet.m_body + sizeof(flv_video_tag),
        //            nalu.data(),
        //            nalu.size());

        //        const int res = RTMP_SendPacket(this->rtmp, &packet, 0);
        //    }

        //    const int res = 1;//RTMP_SendPacket(this->rtmp, &packet, 0);
        //    RTMPPacket_Free(&packet);
        //    if(!res)
        //        throw HR_EXCEPTION(E_UNEXPECTED);
        //}

        start_code_prefix_pos = next_start_code_prefix_pos;
    }

    // send all data
    const uint32_t rtmp_body_size =
        sizeof(flv_tag) + sizeof(flv_video_tag) + (uint32_t)data.size();
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

    const uint32_t avc_packet_type = 1,
        composition_time =
        (uint32_t)((double)(pts - dts) / SECOND_IN_TIME_UNIT * 1000.0) & 0x00ffffff;
    video_tag->avc_video_packet.avc_packet_type_and_composition_time =
        _byteswap_ulong((avc_packet_type << 24) | composition_time);

    assert_(sizeof(flv_tag) + sizeof(flv_video_tag) + (uint32_t)data.size() == rtmp_body_size);
    memcpy(
        packet.data() + sizeof(flv_tag) + sizeof(flv_video_tag),
        data.data(),
        data.size());

    *(uint32_t*)(packet.data() + packet.size() - 4) = _byteswap_ulong(rtmp_body_size - 1);

    const int res = RTMP_Write(this->rtmp, packet.data(), (int)packet.size());
    if(!res)
        throw HR_EXCEPTION(E_UNEXPECTED);




    //// send all nalus
    //if(this->video_headers_sent)
    //{
    //    const uint32_t rtmp_body_size = sizeof(flv_video_tag) + (uint32_t)data.size();
    //    RTMPPacket packet = {0};
    //    if(!RTMPPacket_Alloc(&packet, rtmp_body_size))
    //        throw HR_EXCEPTION(E_UNEXPECTED);

    //    packet.m_packetType = RTMP_PACKET_TYPE_VIDEO;
    //    packet.m_hasAbsTimestamp = TRUE;
    //    packet.m_nChannel = 4;
    //    packet.m_nInfoField2 = this->rtmp->m_stream_id;
    //    packet.m_nBodySize = rtmp_body_size;
    //    packet.m_nTimeStamp = RTMP_GetTime();//(uint32_t)((double)pts / SECOND_IN_TIME_UNIT * 1000.0);
    //    packet.m_headerType = RTMP_PACKET_SIZE_LARGE;

    //    flv_video_tag* video_tag = (flv_video_tag*)packet.m_body;
    //    video_tag->frame_type = key_frame ? 1 : 2;
    //    video_tag->codec_id = 7;

    //    const uint32_t avc_packet_type = 1,
    //        composition_time =
    //        (uint32_t)((double)(pts - dts) / SECOND_IN_TIME_UNIT * 1000.0) & 0x00ffffff;

    //    video_tag->avc_video_packet.avc_packet_type_and_composition_time =
    //        _byteswap_ulong((avc_packet_type << 24) | composition_time);

    //    memcpy(
    //        packet.m_body + sizeof(flv_video_tag),
    //        data.data(),
    //        data.size());

    //    const int res = RTMP_SendPacket(this->rtmp, &packet, 0);
    //    RTMPPacket_Free(&packet);
    //    if(!res)
    //        throw HR_EXCEPTION(E_UNEXPECTED);
    //}
}

void output_rtmp::write_sample(
    bool video, 
    frame_unit fps_num, 
    frame_unit fps_den,
    const CComPtr<IMFSample>& sample)
{
    assert_(sample);

    HRESULT hr = S_OK;
    CComPtr<IMFMediaBuffer> buffer;
    BYTE* buffer_char = nullptr;
    DWORD buffer_len;
    LONGLONG sample_time, sample_duration;

    CHECK_HR(hr = sample->GetSampleTime(&sample_time));
    CHECK_HR(hr = sample->GetSampleDuration(&sample_duration)); sample_duration;
    CHECK_HR(hr = sample->GetBufferByIndex(0, &buffer));
    CHECK_HR(hr = buffer->GetCurrentLength(&buffer_len));

    if(!buffer_len)
        return;

    CHECK_HR(hr = buffer->Lock(&buffer_char, nullptr, nullptr));

    if(video)
    {
        UINT32 key_frame;
        LONGLONG dts;

        CHECK_HR(hr = sample->GetUINT32(MFSampleExtension_CleanPoint, &key_frame));
        dts = MFGetAttributeUINT64(sample, MFSampleExtension_DecodeTimestamp, sample_time);

        try
        {
            const std::string_view data((char*)buffer_char, buffer_len);
            this->send_rtmp_video_packets(data, sample_time, dts, (bool)key_frame);
        }
        catch(streaming::exception err)
        {
            CHECK_HR(hr = err.get_hresult());
        }
        catch(std::exception)
        {
            CHECK_HR(hr = E_UNEXPECTED);
        }
    }
    else
    {
        try
        {
            const std::string_view data((char*)buffer_char, buffer_len);
            this->send_rtmp_audio_packets(data, sample_time);
        }
        catch(streaming::exception err)
        {
            CHECK_HR(hr = err.get_hresult());
        }
        catch(std::exception)
        {
            CHECK_HR(hr = E_UNEXPECTED);
        }
    }

done:
    if(buffer && buffer_char)
        buffer->Unlock();

    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}