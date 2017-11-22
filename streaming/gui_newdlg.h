#pragma once

#include "wtl.h"
#include "control_scene.h"
#include <string>
#include <vector>

class gui_maindlg;

class gui_newdlg : public CDialogImpl<gui_newdlg>
{
private:
    control_scene* scene;
    CComboBox combobox;
    CEdit editbox;
public:
    enum {IDD = IDD_DIALOG_NEW};

    gui_maindlg& wnd_parent;
    std::vector<control_scene::displaycapture_item> displaycaptures;
    std::vector<control_scene::audio_item> audios;
    std::wstring new_scene_name;
    int cursel;
    int new_item;

    explicit gui_newdlg(gui_maindlg&, control_scene*);

    BEGIN_MSG_MAP(gui_newdlg)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_HANDLER(IDOK, BN_CLICKED, OnBnClickedOk)
        COMMAND_HANDLER(IDCANCEL, BN_CLICKED, OnBnClickedCancel)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnBnClickedOk(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedCancel(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnDestroy(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
};