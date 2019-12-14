#include "gui_configdlgs.h"
#include <mfapi.h>
#include <string>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

void gui_configdlg::set_splitter(CStatic& wnd_static)
{
    wnd_static.SetWindowTextW(L"");

    RECT static_rect;
    wnd_static.GetWindowRect(&static_rect);
    const LONG static_width = static_rect.right - static_rect.left;
    wnd_static.SetWindowPos(nullptr, 0, 0, static_width, 1, SWP_NOMOVE | SWP_NOZORDER);

    const LONG style = wnd_static.GetWindowLongW(GWL_STYLE);
    wnd_static.SetWindowLongW(GWL_STYLE, style | SS_ETCHEDHORZ);
}

LRESULT gui_configdlg_general::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    if(this->ctrl_pipeline->is_recording())
        EnumChildWindows(*this, [](HWND hwnd, LPARAM) -> BOOL
            {
                ::EnableWindow(hwnd, FALSE);
                return TRUE;
            }, 0);

    return 0;
}

bool gui_configdlg_video::should_update_settings() const
{
    static_assert(std::is_same_v<decltype(config_video), control_video_config>);
    static_assert(std::is_same_v<
        decltype(config_video), 
        decltype(control_pipeline_config::config_video)>);

    // read the dialog and update the local video config

    // fps num
    {
        CString str;
        this->wnd_fps_num.GetWindowTextW(str);
        this->config_video.fps_num = std::stoi(str.GetString());

        if(this->config_video.fps_num <= 0)
            throw std::invalid_argument("");
    }

    // fps den
    {
        CString str;
        this->wnd_fps_den.GetWindowTextW(str);
        this->config_video.fps_den = std::stoi(str.GetString());

        if(this->config_video.fps_den <= 0)
            throw std::invalid_argument("");
    }

    // video resolution
    {
        CString str, str_width, str_height;
        int start_index = 0;
        this->wnd_video_resolution.GetWindowTextW(str);

        str_width = str.Tokenize(L"x", start_index);
        str_height = str.Mid(start_index);

        this->config_video.width_frame = std::stoi(str_width.GetString());
        this->config_video.height_frame = std::stoi(str_height.GetString());

        if((int)this->config_video.width_frame <= 0 ||
            (int)this->config_video.height_frame <= 0)
            throw std::invalid_argument("");
    }

    // video device
    if(this->wnd_adapter.GetCurSel() == 0)
    {
        this->config_video.adapter_use_default = true;
        this->config_video.adapter = {0};
    }
    else
    {
        this->config_video.adapter_use_default = false;
        this->config_video.adapter = this->adapters.at(this->wnd_adapter.GetCurSel() - 1);
    }

    // encoder
    if(this->wnd_encoder.GetCurSel() == 0)
    {
        this->config_video.encoder_use_default = true;
        this->config_video.encoder = {0};
    }
    else
    {
        this->config_video.encoder_use_default = false;
        this->config_video.encoder = this->encoders.at(this->wnd_encoder.GetCurSel() - 1);
    }

    // bitrate
    {
        CString str;
        this->wnd_bitrate.GetWindowTextW(str);

        this->config_video.bitrate = std::stoi(str.GetString());
        if((int)this->config_video.bitrate <= 0)
            throw std::invalid_argument("");
    }

    // quality vs speed
    {
        CString str;
        this->wnd_quality_vs_speed.GetWindowTextW(str);

        this->config_video.quality_vs_speed = std::stoi(str.GetString());
        // unsigned comparison
        if(this->config_video.quality_vs_speed > 100)
            throw std::invalid_argument("");
    }

    // h264 profile
    switch(this->wnd_mpeg2_profile.GetCurSel())
    {
    case 0:
        this->config_video.h264_video_profile = eAVEncH264VProfile_Simple;
        break;
    case 1:
        this->config_video.h264_video_profile = eAVEncH264VProfile_Main;
        break;
    case 2:
        this->config_video.h264_video_profile = eAVEncH264VProfile_High;
        break;
    default:
        throw std::invalid_argument("");
    }

    return std::memcmp(
        &this->config_video, 
        &this->ctrl_pipeline->get_current_config().config_video,
        sizeof(control_video_config)) != 0;
}

void gui_configdlg_video::populate_encoders_vector_and_combobox(UINT32 flags)
{
    HRESULT hr = S_OK;
    IMFActivate** activate = nullptr;
    UINT count = 0;

    MFT_REGISTER_TYPE_INFO info = {MFMediaType_Video, MFVideoFormat_H264};
    CHECK_HR(hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        flags,
        nullptr,
        &info,
        &activate,
        &count));

    for(UINT i = 0; i < count; i++)
    {
        CLSID clsid;
        UINT32 str_len = 0;

        CHECK_HR(hr = activate[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid));
        activate[i]->GetStringLength(MFT_FRIENDLY_NAME_Attribute, &str_len);

        this->encoders.push_back(clsid);

        if(str_len > 0)
        {
            LPWSTR friendly_name = new WCHAR[str_len + 1];
            friendly_name[str_len] = 0;

            hr = activate[i]->GetString(
                MFT_FRIENDLY_NAME_Attribute,
                friendly_name,
                str_len + 1,
                nullptr);

            if(SUCCEEDED(hr))
                this->wnd_encoder.AddString(friendly_name);
            delete[] friendly_name;

            CHECK_HR(hr);
        }
        else
        {
            this->wnd_encoder.AddString(L"(Unnamed)");
        }
    }

done:
    if(activate)
    {
        for(UINT i = 0; i < count; i++)
            activate[i]->Release();
        CoTaskMemFree(activate);
    }

    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

LRESULT gui_configdlg_video::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    HRESULT hr = S_OK;

    constexpr int dropped_width_increase = 100;

    this->wnd_fps_num.Attach(this->GetDlgItem(IDC_EDIT3));
    this->wnd_fps_den.Attach(this->GetDlgItem(IDC_EDIT4));
    this->wnd_video_resolution.Attach(this->GetDlgItem(IDC_COMBO3));
    this->wnd_mpeg2_profile.Attach(this->GetDlgItem(IDC_COMBO1));
    this->wnd_static_splitter.Attach(this->GetDlgItem(IDC_STATIC1));
    this->wnd_bitrate.Attach(this->GetDlgItem(IDC_EDIT2));
    this->wnd_quality_vs_speed.Attach(this->GetDlgItem(IDC_EDIT5));
    this->wnd_adapter.Attach(this->GetDlgItem(IDC_COMBO2));
    this->wnd_encoder.Attach(this->GetDlgItem(IDC_COMBO4));

    this->wnd_video_resolution.AddString(L"426x240");
    this->wnd_video_resolution.AddString(L"640x360");
    this->wnd_video_resolution.AddString(L"854x480");
    this->wnd_video_resolution.AddString(L"1280x720");
    this->wnd_video_resolution.AddString(L"1920x1080");
    this->wnd_video_resolution.AddString(L"2560x1440");
    this->wnd_video_resolution.AddString(L"3840x2160");

    this->wnd_adapter.SetDroppedWidth(this->wnd_adapter.GetDroppedWidth() + dropped_width_increase);

    this->wnd_encoder.SetDroppedWidth(this->wnd_encoder.GetDroppedWidth() + dropped_width_increase);

    this->wnd_mpeg2_profile.AddString(L"Simple Profile");
    this->wnd_mpeg2_profile.AddString(L"Main Profile");
    this->wnd_mpeg2_profile.AddString(L"High Profile");

    this->set_splitter(this->wnd_static_splitter);

    // populate the fields
    const control_pipeline_config& config = this->ctrl_pipeline->get_current_config();
    this->wnd_fps_num.SetWindowTextW(std::to_wstring(config.config_video.fps_num).c_str());
    this->wnd_fps_den.SetWindowTextW(std::to_wstring(config.config_video.fps_den).c_str());
    this->wnd_video_resolution.SetWindowTextW((std::to_wstring(config.config_video.width_frame) 
        + L"x" + std::to_wstring(config.config_video.height_frame)).c_str());

    // populate video adapter vector
    this->wnd_adapter.AddString(L"System Default");
    {
        CComPtr<IDXGIFactory1> dxgifactory;
        CComPtr<IDXGIAdapter1> dxgiadapter;

        CHECK_HR(hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgifactory));

        for(UINT adapter = 0;
            (hr = dxgifactory->EnumAdapters1(adapter, &dxgiadapter)) != DXGI_ERROR_NOT_FOUND;
            adapter++)
        {
            CHECK_HR(hr);

            DXGI_ADAPTER_DESC1 desc;
            CHECK_HR(hr = dxgiadapter->GetDesc1(&desc));
            this->adapters.push_back(desc.AdapterLuid);

            desc.Description[127] = 0;
            this->wnd_adapter.AddString(desc.Description);

            dxgiadapter = nullptr;
        }
        hr = S_OK;
    }

    // select the video device
    {
        bool found = false;
        int selection = 1;
        for(auto&& item : this->adapters)
        {
            static_assert(std::is_same_v<LUID, std::decay_t<decltype(item)>>);
            static_assert(std::is_same_v<
                std::decay_t<decltype(item)>,
                decltype(config.config_video.adapter)>);

            if(std::memcmp(&item, &config.config_video.adapter, sizeof(LUID)) == 0)
                found = true;
            else if(!found)
                selection++;
        }
        if(!found || config.config_video.adapter_use_default)
            selection = 0;

        this->wnd_adapter.SetCurSel(selection);
    }

    // populate encoder vector
    this->wnd_encoder.AddString(L"System Default");
    this->populate_encoders_vector_and_combobox(MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_HARDWARE);
    this->populate_encoders_vector_and_combobox(MFT_ENUM_FLAG_SORTANDFILTER);

    // select the encoder
    {
        bool found = false;
        int selection = 1;
        for(auto&& item : this->encoders)
        {
            static_assert(std::is_same_v<CLSID, std::decay_t<decltype(item)>>);
            static_assert(std::is_same_v<
                std::decay_t<decltype(item)>,
                decltype(config.config_video.encoder)>);

            if(std::memcmp(&item, &config.config_video.encoder, sizeof(CLSID)) == 0)
                found = true;
            else if(!found)
                selection++;
        }
        if(!found || config.config_video.encoder_use_default)
            selection = 0;

        this->wnd_encoder.SetCurSel(selection);
    }

    // select the h264 profile
    switch(config.config_video.h264_video_profile)
    {
    case eAVEncH264VProfile_Simple:
        this->wnd_mpeg2_profile.SetCurSel(0);
        break;
    case eAVEncH264VProfile_Main:
        this->wnd_mpeg2_profile.SetCurSel(1);
        break;
    case eAVEncH264VProfile_High:
        this->wnd_mpeg2_profile.SetCurSel(2);
        break;
    default:
        throw HR_EXCEPTION(E_UNEXPECTED);
    }

    // populate the bitrate and quality vs speed
    this->wnd_bitrate.SetWindowTextW(std::to_wstring(config.config_video.bitrate).c_str());
    this->wnd_quality_vs_speed.SetWindowTextW(
        std::to_wstring(config.config_video.quality_vs_speed).c_str());

    if(this->ctrl_pipeline->is_recording())
        EnumChildWindows(*this, [](HWND hwnd, LPARAM) -> BOOL
            {
                ::EnableWindow(hwnd, FALSE);
                return TRUE;
            }, 0);

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);

    return 0;
}

bool gui_configdlg_audio::should_update_settings() const
{
    static_assert(std::is_same_v<
        decltype(config_audio),
        control_audio_config>);
    static_assert(std::is_same_v<
        decltype(config_audio),
        decltype(control_pipeline_config::config_audio)>);

    // read the dialog and update the local audio config

    // sample rate
    switch(this->wnd_sample_rate.GetCurSel())
    {
    case 0:
        this->config_audio.sample_rate = 44100;
        break;
    case 1:
        this->config_audio.sample_rate = 48000;
        break;
    default:
        throw std::invalid_argument("");
    }

    // channels
    switch(this->wnd_channels.GetCurSel())
    {
    case 0:
        this->config_audio.channels = 2;
        break;
    default:
        throw std::invalid_argument("");
    }

    // bitrate
    switch(this->wnd_bitrate.GetCurSel())
    {
    case 0:
        this->config_audio.bitrate = transform_aac_encoder::rate_96;
        break;
    case 1:
        this->config_audio.bitrate = transform_aac_encoder::rate_128;
        break;
    case 2:
        this->config_audio.bitrate = transform_aac_encoder::rate_160;
        break;
    case 3:
        this->config_audio.bitrate = transform_aac_encoder::rate_196;
        break;
    default:
        throw std::invalid_argument("");
    }

    // aac profile
    switch(this->wnd_aac_profile.GetCurSel())
    {
    case 0:
        this->config_audio.profile_level_indication = 0x29;
        break;
    case 1:
        this->config_audio.profile_level_indication = 0x2a;
        break;
    case 2:
        this->config_audio.profile_level_indication = 0x2b;
        break;
    case 3:
        this->config_audio.profile_level_indication = 0x2c;
        break;
    case 4:
        this->config_audio.profile_level_indication = 0x2e;
        break;
    default:
        throw std::invalid_argument("");
    }

    return std::memcmp(
        &this->config_audio,
        &this->ctrl_pipeline->get_current_config().config_audio,
        sizeof(control_audio_config)) != 0;
}

LRESULT gui_configdlg_audio::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    this->wnd_sample_rate.Attach(this->GetDlgItem(IDC_COMBO3));
    this->wnd_channels.Attach(this->GetDlgItem(IDC_COMBO4));
    this->wnd_bitrate.Attach(this->GetDlgItem(IDC_COMBO1));
    this->wnd_aac_profile.Attach(this->GetDlgItem(IDC_COMBO5));
    this->wnd_static_splitter.Attach(this->GetDlgItem(IDC_STATIC1));

    this->wnd_sample_rate.AddString(L"44 100 Hz");
    this->wnd_sample_rate.AddString(L"48 000 Hz");

    this->wnd_channels.AddString(L"2 (Stereo)");

    this->wnd_bitrate.AddString(L"96");
    this->wnd_bitrate.AddString(L"128");
    this->wnd_bitrate.AddString(L"160");
    this->wnd_bitrate.AddString(L"196");

    this->wnd_aac_profile.AddString(L"AAC Profile L2");
    this->wnd_aac_profile.AddString(L"AAC Profile L4");
    this->wnd_aac_profile.AddString(L"AAC Profile L5");
    this->wnd_aac_profile.AddString(L"High Efficiency v1 AAC Profile L2");
    this->wnd_aac_profile.AddString(L"High Efficiency v1 AAC Profile L4");

    this->set_splitter(this->wnd_static_splitter);

    // populate the fields
    const control_pipeline_config& config = this->ctrl_pipeline->get_current_config();
    if(config.config_audio.sample_rate == 44100)
        this->wnd_sample_rate.SetCurSel(0);
    else if(config.config_audio.sample_rate == 48000)
        this->wnd_sample_rate.SetCurSel(1);
    else
        throw HR_EXCEPTION(E_UNEXPECTED);

    if(config.config_audio.channels == 2)
        this->wnd_channels.SetCurSel(0);
    else
        throw HR_EXCEPTION(E_UNEXPECTED);

    switch(config.config_audio.bitrate)
    {
    case transform_aac_encoder::rate_96:
        this->wnd_bitrate.SetCurSel(0);
        break;
    case transform_aac_encoder::rate_128:
        this->wnd_bitrate.SetCurSel(1);
        break;
    case transform_aac_encoder::rate_160:
        this->wnd_bitrate.SetCurSel(2);
        break;
    case transform_aac_encoder::rate_196:
        this->wnd_bitrate.SetCurSel(3);
        break;
    default:
        throw HR_EXCEPTION(E_UNEXPECTED);
    }

    switch(config.config_audio.profile_level_indication)
    {
    case 0x29:
        this->wnd_aac_profile.SetCurSel(0);
        break;
    case 0x2a:
        this->wnd_aac_profile.SetCurSel(1);
        break;
    case 0x2b:
        this->wnd_aac_profile.SetCurSel(2);
        break;
    case 0x2c:
        this->wnd_aac_profile.SetCurSel(3);
        break;
    case 0x2e:
        this->wnd_aac_profile.SetCurSel(4);
        break;
    default:
        throw HR_EXCEPTION(E_UNEXPECTED);
    }

    if(this->ctrl_pipeline->is_recording())
        EnumChildWindows(*this, [](HWND hwnd, LPARAM) -> BOOL
            {
                ::EnableWindow(hwnd, FALSE);
                return TRUE;
            }, 0);

    return 0;
}