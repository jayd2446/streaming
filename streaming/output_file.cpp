#include "output_file.h"
#include "assert.h"
#include <iostream>
#include <Mferror.h>

#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfreadwrite.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

output_file::output_file() : stopped(false)
{
}

output_file::~output_file()
{
    // clean stop
    this->force_stop();
}

void output_file::initialize(
    bool null_file,
    ATL::CWindow recording_initiator,
    const CComPtr<IMFMediaType>& video_type,
    const CComPtr<IMFMediaType>& audio_type)
{
    if(!null_file)
    {
        HRESULT hr = S_OK;
        CComPtr<IMFAttributes> sink_writer_attributes;

        this->recording_initiator = recording_initiator;
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
        // disable the throttling so that low fps doesn't cause the audio to throttle
        CHECK_HR(hr = sink_writer_attributes->SetUINT32(
            MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));

        // create sink writer
        CHECK_HR(hr = MFCreateSinkWriterFromMediaSink(
            this->mpeg_media_sink, sink_writer_attributes, &this->writer));

        // start accepting data
        CHECK_HR(hr = this->writer->BeginWriting());
    
    done:
        if(FAILED(hr))
            throw HR_EXCEPTION(hr);
    }
    else
        this->stopped = true;
}

void output_file::write_sample(bool video, const CComPtr<IMFSample>& sample)
{
    if(this->stopped)
        return;

    HRESULT hr = S_OK;
    CHECK_HR(hr = this->writer->WriteSample(video ? 0 : 1, sample));

done:
    if(!this->stopped && FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void output_file::force_stop()
{
    if(this->stopped)
        return;
    this->stopped = true;

    scoped_lock lock(this->stop_mutex);

    // finalize
    HRESULT hr = S_OK;
    CHECK_HR(hr = this->writer->Finalize());

done:
    this->writer.Release();
    this->mpeg_media_sink->Shutdown();
    /*Sleep(5000);*/
    // sendnotifymessage returns immediately when the message pump is in another thread;
    // the message is dispached in getmessage, and the getmessage doesn't yield control
    // to the message pump;
    // postmessage will cause the message pump to run
    /*
    A common programming error is to assume that the PostMessage function always posts a message.
    */
    this->recording_initiator.SendNotifyMessageW(RECORDING_STOPPED_MESSAGE, 1);

    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}