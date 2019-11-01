#include "source_vidcap.h"
#include "transform_videomixer.h"
#include "assert.h"
#include <thread>
#include <iostream>

#undef max
#undef min

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

struct source_vidcap::source_reader_callback_t : public IMFSourceReaderCallback2, IUnknownImpl
{
    std::weak_ptr<source_vidcap> source;
    std::mutex on_read_sample_mutex;

    explicit source_reader_callback_t(const source_vidcap_t& source) : source(source) {}
    void on_error(const source_vidcap_t& source);

    ULONG STDMETHODCALLTYPE AddRef() {return IUnknownImpl::AddRef();}
    ULONG STDMETHODCALLTYPE Release() {return IUnknownImpl::Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv)
    {
        if(!ppv)
            return E_POINTER;
        if(riid == __uuidof(IUnknown))
            *ppv = static_cast<IUnknown*>(this);
        else if(riid == __uuidof(IMFSourceReaderCallback2))
            *ppv = static_cast<IMFSourceReaderCallback2*>(this);
        else if(riid == __uuidof(IMFSourceReaderCallback))
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
        else
        {
            *ppv = NULL;
            return E_NOINTERFACE;
        }

        this->AddRef();
        return S_OK;
    }

    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex,
        DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample);
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*)
    {
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD)
    {
        return S_OK;
    }
    STDMETHODIMP OnStreamError(DWORD /*dwStreamIndex*/, HRESULT hr)
    {
        streaming::check_for_errors();

        source_vidcap_t source = this->source.lock();
        if(!source)
            return S_OK;

        if(FAILED(hr) && hr != MF_E_SHUTDOWN)
            this->on_error(source);

        return S_OK;
    }
    STDMETHODIMP OnTransformChange()
    {
        streaming::check_for_errors();
        return S_OK;
    }
};

void source_vidcap::source_reader_callback_t::on_error(const source_vidcap_t& source)
{
    streaming::check_for_errors();
    source->set_broken();
}

HRESULT source_vidcap::source_reader_callback_t::OnReadSample(HRESULT hr, DWORD /*stream_index*/,
    DWORD flags, LONGLONG timestamp, IMFSample* sample)
{
    // TODO: these callbacks should be wrapped to try exception block

    streaming::check_for_errors();

    scoped_lock lock(this->on_read_sample_mutex);

    source_vidcap_t source = this->source.lock();
    if(!source)
        return S_OK;

    CHECK_HR(hr);

    if(source->reset_size.exchange(false))
    {
        // resize
        source->ctrl_pipeline->run_in_gui_thread([this](const control_class_t& pipeline)
            {
                pipeline->activate();
            });
    }

    // TODO: https://docs.microsoft.com/en-us/windows/desktop/medfound/handling-video-device-loss

    //  this is assumed to be singlethreaded
    if((flags & MF_SOURCE_READERF_ENDOFSTREAM) || (flags & MF_SOURCE_READERF_ERROR))
    {
        std::cout << "end of stream" << std::endl;
        source->set_broken();
    }
    if(flags & MF_SOURCE_READERF_NEWSTREAM)
    {
        std::cout << "new stream" << std::endl;
    }
    if(flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)
    {
        std::cout << "native mediatype changed" << std::endl;
    }
    if(flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
    {
        std::cout << "current mediatype changed" << std::endl;

        source->output_type = NULL;

        CHECK_HR(hr = source->source_reader->GetCurrentMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            &source->output_type));
        {
            scoped_lock lock(source->size_mutex);
            CHECK_HR(hr = MFGetAttributeSize(source->output_type, MF_MT_FRAME_SIZE,
                &source->frame_width, &source->frame_height));
        }

        // reactivate the current scene; it will update the control_vidcap with 
        // a possible new frame size
        source->ctrl_pipeline->run_in_gui_thread([this](const control_class_t& pipeline)
            {
                pipeline->activate();
            });
    }
    if(flags & MF_SOURCE_READERF_STREAMTICK)
    {
        std::cout << "stream tick" << std::endl;
    }

    if(sample)
    {
        CComPtr<IMFMediaBuffer> buffer;
        CComPtr<IMFDXGIBuffer> dxgi_buffer;
        CComPtr<ID3D11Texture2D> texture;
        media_clock_t clock = source->session->get_clock();
        media_sample_video_mixer_frame frame;
        D3D11_TEXTURE2D_DESC desc;

        if(!clock)
        {
            std::cout << "clock was not initialized" << std::endl;
            goto done;
        }

        CHECK_HR(hr = sample->GetBufferByIndex(0, &buffer));
        CHECK_HR(hr = buffer->QueryInterface(&dxgi_buffer));
        CHECK_HR(hr = dxgi_buffer->GetResource(__uuidof(ID3D11Texture2D), (LPVOID*)&texture));
        texture->GetDesc(&desc);

        CHECK_HR(hr = sample->GetSampleTime(&timestamp));

        // TODO: use this in source_wasapi aswell(maybe)

        // make frame
        const frame_unit fps_num = source->session->frame_rate_num, 
            fps_den = source->session->frame_rate_den;
        const time_unit real_timestamp = clock->system_time_to_clock_time(timestamp),
            calculated_timestamp = convert_to_time_unit(source->next_frame_pos, fps_num, fps_den),
            frame_interval = convert_to_time_unit(1, fps_num, fps_den);

        // reset the next frame pos if not set or the timestamps have drifted too far apart
        if(source->next_frame_pos < 0 || 
            std::abs(real_timestamp - calculated_timestamp) > (frame_interval / 2))
        {
            std::cout << "source_vidcap time base reset" << std::endl;
            source->next_frame_pos = convert_to_frame_unit(real_timestamp, fps_num, fps_den);
        }

        frame.pos = source->next_frame_pos++;
        frame.dur = 1;
        frame.buffer = source->acquire_buffer(desc);

        // texture must be copied from sample so that media foundation can work correctly;
        // media foundation has a limit for pooled samples and if it is reached
        // media foundation begins to stall
        {
            using scoped_lock = std::lock_guard<std::recursive_mutex>;
            scoped_lock lock(*source->context_mutex);
            source->d3d11devctx->CopyResource(frame.buffer->texture, texture);
        }

        frame.params.source_rect.top = frame.params.source_rect.left = 0.f;
        frame.params.source_rect.right = (FLOAT)source->frame_width;
        frame.params.source_rect.bottom = (FLOAT)source->frame_height;
        frame.params.dest_rect = frame.params.source_rect;
        frame.params.source_m = frame.params.dest_m = D2D1::Matrix3x2F::Identity();

        // add the frame
        {
            scoped_lock lock(source->source_helper_mutex);
            source->source_helper.add_new_sample(frame);
        }
    }

    CHECK_HR(hr = source->queue_new_capture());

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        this->on_error(source);

    return S_OK;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


source_vidcap::source_vidcap(const media_session_t& session, context_mutex_t context_mutex) :
    source_base(session),
    context_mutex(context_mutex),
    buffer_pool_texture(new buffer_pool_texture_t),
    frame_width(0), frame_height(0), 
    next_frame_pos(-1),
    is_capture_initialized(false), is_helper_initialized(false),
    reset_size(false)
{
}

source_vidcap::~source_vidcap()
{
    buffer_pool_texture_t::scoped_lock lock(this->buffer_pool_texture->mutex);
    this->buffer_pool_texture->dispose();
}

void source_vidcap::initialize_buffer(const media_buffer_texture_t& buffer,
    const D3D11_TEXTURE2D_DESC& desc)
{
    D3D11_TEXTURE2D_DESC desc_ = desc;
    desc_.MipLevels = 1;
    desc_.ArraySize = 1;
    desc_.SampleDesc.Count = 1;
    desc_.SampleDesc.Quality = 0;
    desc_.CPUAccessFlags = 0;
    desc_.MiscFlags = 0;
    desc_.Usage = D3D11_USAGE_DEFAULT;
    desc_.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc_.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    buffer->initialize(this->d3d11dev, desc_, NULL);
}

media_buffer_texture_t source_vidcap::acquire_buffer(const D3D11_TEXTURE2D_DESC& desc)
{
    buffer_pool_texture_t::scoped_lock lock(this->buffer_pool_texture->mutex);
    media_buffer_texture_t buffer = this->buffer_pool_texture->acquire_buffer();
    lock.unlock();

    this->initialize_buffer(buffer, desc);
    return buffer;
}

source_vidcap::stream_source_base_t source_vidcap::create_derived_stream()
{
    return stream_vidcap_t(new stream_vidcap(this->shared_from_this<source_vidcap>()));
}

bool source_vidcap::get_samples_end(time_unit request_time, frame_unit& end) const
{
    scoped_lock lock(this->source_helper_mutex);
    return this->source_helper.get_samples_end(request_time, end);
}

void source_vidcap::make_request(request_t& request, frame_unit frame_end)
{
    scoped_lock lock(this->source_helper_mutex);

    request.sample.args = std::make_optional<media_component_videomixer_args>();
    media_component_videomixer_args& args = *request.sample.args;

    args.frame_end = frame_end;

    media_sample_video_mixer_frames_t sample = this->source_helper.make_sample(frame_end);
    if(args.sample)
        sample->move_frames_to(args.sample.get(), frame_end);
    else
        args.sample = sample;

    // the sample must not be empty
    assert_(!args.sample->frames.empty());
}

void source_vidcap::dispatch(request_t& request)
{
    this->session->give_sample(request.stream, request.sample.args.has_value() ?
        &(*request.sample.args) : NULL, request.rp);
}

HRESULT source_vidcap::queue_new_capture()
{
    // queue_new_capture is called from on_component_start and initialize_async;
    // readsample itself is only called once

    if(!this->is_capture_initialized || !this->is_helper_initialized)
        return S_OK;

    HRESULT hr;
    CHECK_HR(hr = this->source_reader->ReadSample(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0, NULL, NULL, NULL, NULL));

done:
    return hr;
}

void source_vidcap::initialize_async()
{
    HRESULT hr = S_OK;
    UINT32 fps_num, fps_den;

    CComPtr<IMFAttributes> attributes;
    CComPtr<IMFMediaType> output_type;

    CHECK_HR(hr = MFCreateDXGIDeviceManager(&this->reset_token, &this->devmngr));
    CHECK_HR(hr = this->devmngr->ResetDevice(this->d3d11dev, this->reset_token));

    // configure device source
    CHECK_HR(hr = MFCreateAttributes(&attributes, 2));
    CHECK_HR(hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
    CHECK_HR(hr = attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
        this->symbolic_link.c_str()));

    // configure source reader
    CHECK_HR(hr = MFCreateAttributes(&this->source_reader_attributes, 1));
    CHECK_HR(hr = this->source_reader_attributes->SetUINT32(
        MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    CHECK_HR(hr = this->source_reader_attributes->SetUnknown(
        MF_SOURCE_READER_D3D_MANAGER, this->devmngr));
    CHECK_HR(hr = this->source_reader_attributes->SetUINT32(
        MF_READWRITE_DISABLE_CONVERTERS, FALSE));
    CHECK_HR(hr = this->source_reader_attributes->SetUINT32(
        MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
    this->source_reader_callback.Attach(
        new source_reader_callback_t(this->shared_from_this<source_vidcap>()));
    CHECK_HR(hr = this->source_reader_attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,
        this->source_reader_callback));

    // create device source and source reader
    CHECK_HR(hr = MFCreateDeviceSource(attributes, &this->device));
    CHECK_HR(hr = MFCreateSourceReaderFromMediaSource(this->device,
        this->source_reader_attributes, &this->source_reader));

    // TODO: decide if source reader should convert the frame rate;
    // converting to higher fps causes unnecessary vram usage

    // set the output format for source reader
    CHECK_HR(hr = MFCreateMediaType(&output_type));
    CHECK_HR(hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32));
    // set the frame rate converter
    CHECK_HR(hr = MFSetAttributeRatio(output_type, MF_MT_FRAME_RATE,
        (UINT32)this->session->frame_rate_num,
        (UINT32)this->session->frame_rate_den));
    // TODO: decide if should set frame size
    CHECK_HR(hr = output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK_HR(hr = MFSetAttributeRatio(output_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

    CHECK_HR(hr = this->source_reader->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        NULL, output_type));
    CHECK_HR(hr = this->source_reader->GetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        &this->output_type));
    {
        scoped_lock lock(this->size_mutex);
        CHECK_HR(hr = MFGetAttributeSize(this->output_type, MF_MT_FRAME_SIZE,
            &this->frame_width, &this->frame_height));
    }
    CHECK_HR(hr = MFGetAttributeRatio(this->output_type, MF_MT_FRAME_RATE, &fps_num, &fps_den));

    assert_(fps_num == (UINT32)this->session->frame_rate_num);
    assert_(fps_den == (UINT32)this->session->frame_rate_den);

    // request a resize
    this->reset_size = true;

    // synchronization needs to be done so that the readsample isn't called twice
    {
        source_vidcap::scoped_lock lock(this->queue_new_capture_mutex);
        this->is_capture_initialized = true;
        CHECK_HR(hr = this->queue_new_capture());
    }

    // TODO: failure should be signalled to control_vidcap in some way

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void source_vidcap::initialize(const control_class_t& ctrl_pipeline,
    const CComPtr<ID3D11Device>& d3d11dev,
    const CComPtr<ID3D11DeviceContext>& d3d11devctx,
    const std::wstring& symbolic_link)
{
    assert_(!this->device);

    this->source_base::initialize(ctrl_pipeline);

    this->d3d11dev = d3d11dev;
    this->d3d11devctx = d3d11devctx;
    this->symbolic_link = symbolic_link;

    std::thread(&source_vidcap::initialize_async, this->shared_from_this<source_vidcap>()).detach();
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_vidcap::stream_vidcap(const source_vidcap_t& source) :
    stream_source_base(source),
    source(source)
{
}

void stream_vidcap::on_component_start(time_unit t)
{
    this->source->source_helper.initialize(convert_to_frame_unit(t,
        this->source->session->frame_rate_num,
        this->source->session->frame_rate_den),
        this->source->session->frame_rate_num,
        this->source->session->frame_rate_den);

    HRESULT hr;
    {
        source_vidcap::scoped_lock lock(this->source->queue_new_capture_mutex);
        this->source->is_helper_initialized = true;
        CHECK_HR(hr = this->source->queue_new_capture());
    }

done:
    if(FAILED(hr) && hr != MF_E_SHUTDOWN)
        throw HR_EXCEPTION(hr);
}