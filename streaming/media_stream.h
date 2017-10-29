#pragma once
#include "media_sample.h"
#include "enable_shared_from_this.h"

// media stream object is needed for stream type info;
// mediatypehandler is used to check if the stream supports a format and to
// set a format for the stream;
// format is represented as mediatype

// (formats for the streams are changed by creating new topologies)

class media_topology;
struct request_packet;

class media_stream : public virtual enable_shared_from_this
{
public:
    enum result_t
    {
        OK,
        // TODO: rename fatal error to topology not found
        // the topology encountered an unrecoverable error
        FATAL_ERROR
    };
public:
    virtual ~media_stream() {}

    // requests samples from media session or processes processes
    // samples if there are any;
    // implements input stream functionality
    virtual result_t request_sample(request_packet&, const media_stream* previous_stream) = 0;
    // processes the new sample and optionally calls media_session::give_sample;
    // implements output stream functionality;
    // sample view can be NULL
    virtual result_t process_sample(
        const media_sample_view_t&, request_packet&, const media_stream* previous_stream) = 0;

    // TODO: return a list of available formats for the stream
    // (media session will use these to set valid formats for the streams between components)
    // TODO: use media type handler object instead for these
    /*virtual void get_input_formats() = 0;
    virtual void get_output_formats() = 0;*/

    // TODO: output may not be changed before the stream has processed cached samples
    // test is for querying whether the requested format is supported;
    // set_input_format may trigger the change of the output format
    //virtual bool set_input_format(/*mediatype format,*/ bool test) = 0;
    //virtual bool set_output_format(/*mediatype format,*/ bool test) = 0;
};

typedef std::shared_ptr<media_stream> media_stream_t;