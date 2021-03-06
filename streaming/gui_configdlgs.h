#pragma once

#include "wtl.h"
#include "control_pipeline.h"
#include <dxgi.h>
#include <vector>
#include <stdexcept>

class gui_configdlg
{
public:
    control_pipeline_t ctrl_pipeline;

    explicit gui_configdlg(const control_pipeline_t& ctrl_pipeline) :
        ctrl_pipeline(ctrl_pipeline)
    {
    }
    virtual ~gui_configdlg() = default;

    virtual void create(HWND parent) = 0;
    virtual CWindow& get_wnd() = 0;
    // throws logic_error
    // TODO: this function should take reference settings as an argument so that
    // applying a new config and saving a config to disk could be separately
    // compared
    virtual bool should_update_settings() = 0;
    // updates the settings relevant to this config dlg in the full config
    virtual void update_settings(control_pipeline_config&) = 0;

    void set_splitter(CStatic&);
};

class gui_configdlg_general final : 
    public CDialogImpl<gui_configdlg_general>,
    public gui_configdlg
{
private:
    CStatic wnd_static_splitter;
public:
    enum { IDD = IDD_GENERAL_CONFIG };

    using gui_configdlg::gui_configdlg;

    void create(HWND parent) override { this->Create(parent); }
    CWindow& get_wnd() override { return *this; }
    bool should_update_settings() override { return false; }
    void update_settings(control_pipeline_config&) override {}

    BEGIN_MSG_MAP(gui_configdlgs)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
};

class gui_configdlg_video final :
    public CDialogImpl<gui_configdlg_video>,
    public gui_configdlg
{
private:
    control_video_config config_video;

    CEdit wnd_fps_num, wnd_fps_den;
    CComboBox wnd_video_resolution, wnd_mpeg2_profile;
    CStatic wnd_static_splitter;
    CEdit wnd_bitrate, wnd_quality_vs_speed;
    CComboBox wnd_adapter, wnd_encoder;

    std::vector<UINT> adapters;
    std::vector<CLSID> encoders;

    void populate_encoders_vector_and_combobox(UINT32 flags);
public:
    enum { IDD = IDD_VIDEO_CONFIG };

    using gui_configdlg::gui_configdlg;

    void create(HWND parent) override { this->Create(parent); }
    CWindow& get_wnd() override { return *this; }
    bool should_update_settings() override;
    void update_settings(control_pipeline_config& config) override
    { config.config_video = this->config_video; }

    BEGIN_MSG_MAP(gui_configdlg_video)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
};

class gui_configdlg_audio final :
    public CDialogImpl<gui_configdlg_audio>,
    public gui_configdlg
{
private:
    control_audio_config config_audio;

    CComboBox wnd_sample_rate, wnd_channels, wnd_bitrate, wnd_aac_profile;
    CStatic wnd_static_splitter;
public:
    enum { IDD = IDD_AUDIO_CONFIG };

    using gui_configdlg::gui_configdlg;

    void create(HWND parent) override { this->Create(parent); }
    CWindow& get_wnd() override { return *this; }
    bool should_update_settings() override;
    void update_settings(control_pipeline_config& config) override
    { config.config_audio = this->config_audio; }

    BEGIN_MSG_MAP(gui_configdlg_audio)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
};

class gui_configdlg_output final :
    public CDialogImpl<gui_configdlg_output>,
    public gui_configdlg
{
private:
    control_output_config config_output;

    CEdit wnd_output_folder, wnd_output_file_name;
    CEdit wnd_output_ingest_server, wnd_output_stream_key;
    CButton wnd_overwrite_old_file, wnd_showkey;
    CStatic wnd_static_splitter;
    LRESULT password_char;
public:
    enum { IDD = IDD_OUTPUT_CONFIG };

    using gui_configdlg::gui_configdlg;

    void create(HWND parent) override { this->Create(parent); }
    CWindow& get_wnd() override { return *this; }
    bool should_update_settings() override;
    void update_settings(control_pipeline_config& config) override;

    BEGIN_MSG_MAP(gui_configdlg_output)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_HANDLER(IDC_SHOWKEY, BN_CLICKED, OnBnClickedShowkey)
        COMMAND_HANDLER(IDC_OPENFOLDER, BN_CLICKED, OnBnClickedOpenfolder)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedShowkey(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedOpenfolder(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
};