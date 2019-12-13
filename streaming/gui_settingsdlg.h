#pragma once

#include "wtl.h"
#include "gui_configdlgs.h"
#include "control_pipeline.h"
#include <vector>
#include <string>
#include <string_view>

class gui_settingsdlg final :
    public CDialogImpl<gui_settingsdlg>
{
public:
    using settings_page_t = std::pair<std::wstring, gui_configdlg*>;
private:
    control_pipeline_t ctrl_pipeline;
    CTreeViewCtrlEx wnd_settingstree;
    CStatic wnd_static;

    std::vector<settings_page_t> settings_pages;
    std::map<HTREEITEM, settings_page_t> settings_pages_map;
public:
    enum { IDD = IDD_SETTINGSDLG };

    explicit gui_settingsdlg(const control_pipeline_t&);

    void add_settings_pages(const std::vector<settings_page_t>& pages);
    bool should_update_settings() const;
    void update_settings(control_pipeline_config& config);

    BEGIN_MSG_MAP(gui_settingsdlg)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_HANDLER(IDOK, BN_CLICKED, OnBnClickedOk)
        COMMAND_HANDLER(IDCANCEL, BN_CLICKED, OnBnClickedCancel)
        NOTIFY_HANDLER(IDC_SETTINGSTREE, TVN_SELCHANGED, OnSelectionChanged)
        MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnCtlColorStatic)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedOk(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedCancel(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnSelectionChanged(int /*idCtrl*/, LPNMHDR pNMHDR, BOOL& /*bHandled*/);
    LRESULT OnCtlColorStatic(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
};