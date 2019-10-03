#include "media_sample.h"
#include "assert.h"
#include "IUnknownImpl.h"
#include <mfapi.h>
#include <Mferror.h>
#include <initguid.h>
#include <cmath>
#include <atomic>
#include <limits>
#include <algorithm>
#include <iostream>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

//DEFINE_GUID(media_sample_lifetime_tracker_guid,
//    0xd84fe03a, 0xcb44, 0x43ec, 0x10, 0xac, 0x94, 0x00, 0xb, 0xcc, 0xef, 0x38);

frame_unit convert_to_frame_unit(time_unit t, frame_unit frame_rate_num, frame_unit frame_rate_den)
{
    assert_(frame_rate_num >= 0);
    assert_(frame_rate_den > 0);

    const double frame_duration = SECOND_IN_TIME_UNIT / ((double)frame_rate_num / frame_rate_den);
    return (frame_unit)std::round(t / frame_duration);
}

time_unit convert_to_time_unit(frame_unit pos, frame_unit frame_rate_num, frame_unit frame_rate_den)
{
    assert_(frame_rate_num >= 0);
    assert_(frame_rate_den > 0);

    const double frame_duration = SECOND_IN_TIME_UNIT / ((double)frame_rate_num / frame_rate_den);
    return (time_unit)std::round(pos * frame_duration);
}

//class media_sample_lifetime_tracker : public IUnknown, IUnknownImpl
//{
//public:
//    media_buffer_t sample;
//
//    explicit media_sample_lifetime_tracker(const media_buffer_t& sample) : sample(sample) {}
//
//    ULONG STDMETHODCALLTYPE AddRef() { return IUnknownImpl::AddRef(); }
//    ULONG STDMETHODCALLTYPE Release() { return IUnknownImpl::Release(); }
//    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv)
//    {
//        if(!ppv)
//            return E_POINTER;
//        if(riid == __uuidof(IUnknown))
//            *ppv = static_cast<IUnknown*>(this);
//        else
//        {
//            *ppv = NULL;
//            return E_NOINTERFACE;
//        }
//
//        this->AddRef();
//        return S_OK;
//    }
//};
//
//CComPtr<IUnknown> create_lifetime_tracker(const media_buffer_t& sample)
//{
//    // TODO: this
//    CComPtr<IUnknown> ret;
//    ret.Attach(new media_sample_lifetime_tracker(sample));
//
//    return ret;
//}

void media_buffer_memory::initialize(DWORD len)
{
    static constexpr DWORD alignment = 16;

    this->buffer_poolable::initialize();

    HRESULT hr = S_OK;
    // allocate a new buffer
    if(!this->buffer)
    {
        CHECK_HR(hr = MFCreateAlignedMemoryBuffer(len, alignment, &this->buffer));
        CHECK_HR(hr = this->buffer->SetCurrentLength(0));
    }
    else
    {
        DWORD buf_len;
        CHECK_HR(hr = this->buffer->GetMaxLength(&buf_len));

        // allocate a new buffer that satisfies the len
        if(buf_len < len)
        {
            this->buffer = NULL;
            this->initialize(len);
        }
        else
            // the length might be greater than asked for, which is fine
            CHECK_HR(hr = this->buffer->SetCurrentLength(0));
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


void media_buffer_texture::uninitialize()
{
    this->buffer_poolable::uninitialize();

    HRESULT hr = S_OK;
    if(this->texture && this->managed_by_this)
    {
        /*assert_(!this->mf_sample);*/

        CComPtr<IDXGIResource> resource;
        CComPtr<IDXGIDevice2> device;

        // discard the resource
        CHECK_HR(hr = this->texture->QueryInterface(&resource));
        CHECK_HR(hr = resource->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_NORMAL));
        /*CHECK_HR(hr = resource->GetDevice(__uuidof(IDXGIDevice2), (void**)&device));
        CHECK_HR(hr = device->OfferResources(1, &resource.p, DXGI_OFFER_RESOURCE_PRIORITY_LOW));*/
    }
    else
    {
        this->texture = NULL;
        /*this->mf_sample = NULL;*/
    }

    this->managed_by_this = true;

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void media_buffer_texture::initialize(const CComPtr<ID3D11Device>& dev,
    const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* subrsrc, bool reinitialize)
{
    this->buffer_poolable::initialize();

    HRESULT hr = S_OK;

    if(reinitialize)
        this->texture = NULL;

    if(this->texture)
    {
        CComPtr<IDXGIResource> resource;
        CComPtr<IDXGIDevice2> device;

        // reclaim the resource;
        // the texture cannot be used before it is reclaimed again
        /*BOOL was_discarded;*/
        CHECK_HR(hr = this->texture->QueryInterface(&resource));
        CHECK_HR(hr = resource->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM));
        /*CHECK_HR(hr = resource->GetDevice(__uuidof(IDXGIDevice2), (void**)&device));
        CHECK_HR(hr = device->ReclaimResources(1, &resource.p, &was_discarded));*/

        CComPtr<ID3D11Device> old_dev;
        D3D11_TEXTURE2D_DESC old_desc;

        this->texture->GetDesc(&old_desc);
        this->texture->GetDevice(&old_dev);

        // create a new texture if the settings do not match
        if(!(old_dev.IsEqualObject(dev) && (desc.Width == old_desc.Width) &&
            (desc.Height == old_desc.Height) &&
            (desc.Format == old_desc.Format) &&
            (desc.Usage == old_desc.Usage) &&
            (desc.BindFlags == old_desc.BindFlags)))
        {
            std::cout << "texture desc mismatch, reinitializing" << std::endl;
            this->initialize(dev, desc, subrsrc, true);
        }
    }
    else
        CHECK_HR(hr = dev->CreateTexture2D(&desc, subrsrc, &this->texture))

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

void media_buffer_texture::initialize(const CComPtr<ID3D11Texture2D>& texture)
{
    assert_(!this->texture);

    this->buffer_poolable::initialize();

    if(!texture)
        throw HR_EXCEPTION(E_UNEXPECTED);

    this->texture = texture;
    this->managed_by_this = false;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


//bool media_sample_audio_frames::move_or_share_consecutive_frames_to(
//    media_sample_audio_frames* to, frame_unit end, UINT32 block_align, 
//    media_sample_audio_consecutive_frames& elem, bool share)
//{
//    if(elem.pos >= end)
//        return false;
//
//    HRESULT hr = S_OK;
//    bool remove = false;
//    CComPtr<IMFSample> sample;
//    CComPtr<IMFMediaBuffer> buffer, old_buffer;
//    DWORD buflen = 0;
//    media_sample_audio_consecutive_frames new_frames;
//
//    const frame_unit frame_pos = elem.pos;
//    const frame_unit frame_dur = elem.dur;
//    const frame_unit frame_end = frame_pos + frame_dur;
//
//    const frame_unit frame_diff_end = std::max(frame_end - end, 0LL);
//    const DWORD offset_end = (DWORD)frame_diff_end * block_align;
//    const frame_unit new_frame_pos = frame_pos;
//    const frame_unit new_frame_dur = frame_dur - frame_diff_end;
//
//    if(elem.buffer)
//    {
//        old_buffer = elem.buffer;
//        CHECK_HR(hr = old_buffer->GetCurrentLength(&buflen));
//
//        assert_(((int)buflen - (int)offset_end) > 0);
//        CHECK_HR(hr = MFCreateMediaBufferWrapper(old_buffer, 0, buflen - offset_end, &buffer));
//        CHECK_HR(hr = buffer->SetCurrentLength(buflen - offset_end));
//    }
//
//    if(offset_end > 0 && !share)
//    {
//        if(elem.buffer)
//        {
//            // remove the moved part of the old buffer
//            CComPtr<IMFMediaBuffer> new_buffer;
//            CHECK_HR(hr = MFCreateMediaBufferWrapper(
//                old_buffer, buflen - offset_end, offset_end, &new_buffer));
//            CHECK_HR(hr = new_buffer->SetCurrentLength(offset_end));
//            elem.buffer = new_buffer;
//        }
//
//        const frame_unit new_frame_dur = offset_end / block_align;
//        const frame_unit new_frame_pos = frame_pos + frame_dur - new_frame_dur;
//
//        elem.pos = new_frame_pos;
//        elem.dur = new_frame_dur;
//    }
//    else
//        remove = true;
//
//    assert_((elem.memory_host && buffer) || (!elem.memory_host && !elem.buffer));
//    new_frames.memory_host = elem.memory_host;
//    new_frames.buffer = buffer;
//    new_frames.pos = new_frame_pos;
//    new_frames.dur = new_frame_dur;
//
//    if(to)
//    {
//        to->frames.push_back(new_frames);
//        to->end = std::max(to->end, new_frames.pos + new_frames.dur);
//    }
//
//done:
//    if(FAILED(hr))
//        throw HR_EXCEPTION(hr);
//
//    return remove;
//}
//
//bool media_sample_audio_frames::move_frames_to(media_sample_audio_frames* to,
//    frame_unit end, UINT32 block_align)
//{
//    assert_(this->end == 0 || !this->frames.empty());
//
//    bool moved = false;
//
//    // the media_sample_audio_consecutive_frames should be lightweight, because it seems that
//    // the value is moved within the container
//    // https://en.wikipedia.org/wiki/Erase%E2%80%93remove_idiom
//    this->frames.erase(std::remove_if(this->frames.begin(), this->frames.end(), 
//        [&](media_sample_audio_consecutive_frames& elem)
//    {
//        assert_(elem.dur > 0);
//        // TODO: sample could have a flag that indicates whether the frames are ordered,
//        // so that the whole list doesn't need to be iterated;
//        // this sample would retain the ordered flag only if this was null when
//        // frames were moved
//        if(elem.pos >= end)
//            return false;
//
//        moved = true;
//
//        return move_or_share_consecutive_frames_to(to, end, block_align, elem, false);
//    }), this->frames.end());
//
//    if(end >= this->end)
//    {
//        assert_(this->frames.empty());
//        // set the end to 'undefined' state
//        this->end = 0;
//    }
//
//    return moved;
//}

//bool media_sample_audio_frames::share_frames_to(
//    media_sample_audio_frames* to, frame_unit end, UINT32 block_align) const
//{
//    assert_(this->end == 0 || !this->frames.empty());
//
//    bool shared = false;
//
//    for(auto&& elem : this->frames)
//    {
//        if(!elem.buffer)
//            throw HR_EXCEPTION(E_UNEXPECTED);
//
//        if(elem.pos >= end)
//            continue;
//
//        shared = true;
//
//        move_or_share_consecutive_frames_to(to, end, block_align,
//            const_cast<media_sample_audio_consecutive_frames&>(elem), true);
//    }
//
//    return shared;
//}