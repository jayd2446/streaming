#pragma once
#include "media_sample.h"
#include "AsyncCallback.h"
#include "IUnknownImpl.h"
#include <atlbase.h>

// media stream object is needed for stream type info;
// mediatypehandler is used to check if the stream supports a format and to
// set a format for the stream;
// format is represented as mediatype

class media_topology;
struct request_packet;

// media stream must use com's reference counting so that it works correctly
// with media foundation's async callbacks;
// cannot be used in conjuction with shared_ptr
class media_stream : public virtual IUnknownImpl
{
public:
    enum result_t
    {
        OK,
        // process_sample needs more input to process output;
        // this value is informal, media session won't take any actions
        NEED_MORE_INPUT,
        // process_sample cannot accept the input;
        // the sample will be simply dropped
        DROP_SAMPLE,
        // process_sample requests to change the output format before calling process_sample again
        CHANGE_FORMAT,
        // the topology encountered an unrecoverable error
        FATAL_ERROR
    };
private:
    //media_topology_node_t component;
    //// used to check if this node is already included in a topology and
    //// for the media session to faciliate data flow (part of node functionality)
    //media_topology_t associated_topology;
public:
    virtual ~media_stream() {}

    // requests samples from media session or processes processes
    // samples if there are any;
    // implements input stream functionality
    virtual result_t request_sample(request_packet&) = 0;
    // processes the new sample and optionally calls media_session::give_sample;
    // implements output stream functionality
    virtual result_t process_sample(const media_sample_t&, request_packet&) = 0;

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

    // TODO: these must be moved to child class
    // IUnknown
    /*ULONG STDMETHODCALLTYPE AddRef() {return IUnknownImpl::AddRef();}
    ULONG STDMETHODCALLTYPE Release() {return IUnknownImpl::Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) {return E_NOTIMPL;}*/
};

typedef CComPtr<media_stream> media_stream_t;