#include "output_file.h"
#include <Mferror.h>

#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfreadwrite.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

output_file::output_file() : stopped_signal(NULL)
{
}

output_file::~output_file()
{
    // finalize
    HRESULT hr = S_OK;
    CHECK_HR(hr = this->writer->Finalize());

done:
    this->writer.Release();
    this->mpeg_media_sink->Shutdown();
    SetEvent(this->stopped_signal);

    // TODO: failure should be signaled through the stopped signal
    if(FAILED(hr))
        throw std::exception();
}

void output_file::initialize(HANDLE stopped_signal,
    const CComPtr<IMFMediaType>& video_type,
    const CComPtr<IMFMediaType>& audio_type)
{
    HRESULT hr = S_OK;
    CComPtr<IMFAttributes> sink_writer_attributes;

    this->stopped_signal = stopped_signal;
    this->video_type = video_type;
    this->audio_type = audio_type;

    // create file
    CHECK_HR(hr = MFCreateFile(
        MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, 
        L"test.mp4", &this->byte_stream));

    // create mpeg 4 media sink
    CHECK_HR(hr = MFCreateMPEG4MediaSink(
        this->byte_stream, this->video_type, this->audio_type, &this->mpeg_media_sink));

    // configure the sink writer
    CHECK_HR(hr = MFCreateAttributes(&sink_writer_attributes, 1));
    CHECK_HR(hr = sink_writer_attributes->SetGUID(
        MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));

    // create sink writer
    CHECK_HR(hr = MFCreateSinkWriterFromMediaSink(
        this->mpeg_media_sink, sink_writer_attributes, &this->writer));

    // start accepting data
    CHECK_HR(hr = this->writer->BeginWriting());

done:
    if(FAILED(hr))
        throw std::exception();
}

void output_file::write_sample(bool video, const CComPtr<IMFSample>& sample)
{
    HRESULT hr = S_OK;
    CHECK_HR(hr = this->writer->WriteSample(video ? 0 : 1, sample));

done:
    if(FAILED(hr))
        throw std::exception();
}