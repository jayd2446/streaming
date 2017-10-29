#include "source_audio.h"
#include "IUnknownImpl.h"
//#include <initguid.h>
#include <mmdeviceapi.h>
#include <mfidl.h>
#include <Audioclient.h>

#pragma comment(lib, "Mfuuid.lib")

#define CHECK_HR(hr_) {if(FAILED(hr_)) goto done;}

class source_audio::source_reader_callback : public IMFSourceReaderCallback, public IUnknownImpl
{
private:
    std::weak_ptr<source_audio> parent;
    std::recursive_mutex callback_mutex;
public:
    explicit source_reader_callback(const source_audio_t& parent) : parent(parent) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() {return IUnknownImpl::AddRef();}
    ULONG STDMETHODCALLTYPE Release() {return IUnknownImpl::Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) 
    {
        static const QITAB qit[] =
        {
            QITABENT(source_reader_callback, IMFSourceReaderCallback),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    // IMFSourceReaderCallback
    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex, 
        DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample *pSample);
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) {return S_OK;}
    STDMETHODIMP OnFlush(DWORD) {return S_OK;}
};


#include <iostream>

HRESULT source_audio::source_reader_callback::OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex, 
    DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample *pSample)
{
    scoped_lock lock(this->callback_mutex);
    
    source_audio_t source = this->parent.lock();
    if(!source)
        return S_OK;

    // AUDCLNT_E_WRONG_ENDPOINT_TYPE;

    if(FAILED(hrStatus) || dwStreamFlags == MF_SOURCE_READERF_ERROR)
        throw std::exception();

    if(pSample)
    {
        scoped_lock lock(source->samples_mutex);
        /*source->samples.push(std::make_pair(llTimestamp, pSample));*/
        std::cout << llTimestamp << std::endl;

        // get new data
        if(FAILED(source->source_reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, NULL, NULL, NULL)))
            throw std::exception();
    }

    return S_OK;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


source_audio::source_audio(const media_session_t& session) : ::media_source(session)
{
}

source_audio::~source_audio()
{
    if(this->media_source)
        this->media_source->Shutdown();
}

HRESULT source_audio::create_source_reader(const PROPVARIANT& format)
{
    HRESULT hr = S_OK;

    CComPtr<IMFMediaType> native_type;
    CComPtr<IMFAttributes> attributes;
    CHECK_HR(hr = MFCreateAttributes(&attributes, 1));

    // create source reader callback
    this->source_reader_cb.Attach(new source_reader_callback(this->shared_from_this<source_audio>()));

    // create source reader
    CHECK_HR(hr = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, this->source_reader_cb));
    CHECK_HR(hr = attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS , TRUE));
    CHECK_HR(hr = attributes->SetUINT32(MF_LOW_LATENCY, TRUE));

    CHECK_HR(hr = 
        MFCreateSourceReaderFromMediaSource(this->media_source, attributes, &this->source_reader));

    // create the native type
    CHECK_HR(hr = MFCreateMediaType(&native_type));
    CHECK_HR(hr = MFInitMediaTypeFromWaveFormatEx(
        native_type, (WAVEFORMATEX*)format.blob.pBlobData, format.blob.cbSize));

    // set the default media type
    CHECK_HR(hr = this->source_reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, native_type));
    /*CHECK_HR(hr = this->source_reader->GetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, &this->source_media_type));*/

    /*CHECK_HR(hr = this->source_media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    CHECK_HR(hr = this->source_media_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * 16 / 8));*/

done:
    return hr;
}

HRESULT source_audio::initialize()
{
    HRESULT hr = S_OK;

    CComPtr<IMMDeviceEnumerator> device_enumerator;
    CComPtr<IMMDevice> device;
    LPWSTR id = NULL;
    PROPVARIANT device_format;
    CComPtr<IPropertyStore> device_props;
    PropVariantInit(&device_format);

    CComPtr<IMFAttributes> attributes;

    /*CComPtr<IMFAttributes> config;
    IMFActivate** devices;
    UINT32 count = 0;

    CHECK_HR(hr = MFCreateAttributes(&config, 1));
    CHECK_HR(hr = config->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID));
    CHECK_HR(hr = MFEnumDeviceSources(config, &devices, &count));

    for(int i = 0; i < count; i++)
    {
        LPWSTR name;
        UINT32 len;
        CHECK_HR(hr = devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len));
        name = NULL;
    }*/

    // get the default audio rendering endpoint device
    CHECK_HR(hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, 
        __uuidof(IMMDeviceEnumerator), (void**)&device_enumerator));
    CHECK_HR(hr = device_enumerator->GetDefaultAudioEndpoint(
        eRender, eConsole, &device));

    // get the device format
    CHECK_HR(hr = device->OpenPropertyStore(STGM_READ, &device_props));
    CHECK_HR(hr = device_props->GetValue(PKEY_AudioEngine_DeviceFormat, &device_format));

    // get the endpoint id
    CHECK_HR(hr = device->GetId(&id));

    CHECK_HR(hr = MFCreateAttributes(&attributes, 2));
    CHECK_HR(hr = attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID));

    // set the endpoint id
    CHECK_HR(hr = attributes->SetString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID,
        id));

    // create device
    CHECK_HR(hr = MFCreateDeviceSource(attributes, &this->media_source));
    CHECK_HR(hr = this->create_source_reader(device_format));

    // start the data capture
    CHECK_HR(hr = this->source_reader->ReadSample(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, NULL, NULL, NULL));

done:
    if(id)
        CoTaskMemFree(id);

    if(FAILED(hr))
        throw std::exception();

    return hr;
}