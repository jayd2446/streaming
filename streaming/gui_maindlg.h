#pragma once

#include "wtl.h"

/*
The ATL Object Wizard does not provide wizard support to use CWindowImpl (but it
does for dialog boxes). It is easy enough to use it manually. You must create a C++
class which derives from CWindowImpl, write a constructor which calls Create and
a destructor which calls DestroyWindow, and add a message map which implements
ProcessWindowMessage. 
*/

class gui_frame;

class gui_maindlg : public CDialogImpl<gui_maindlg>
{
public:
    enum new_item_t
    {
        NEW_SCENE, NEW_VIDEO, NEW_AUDIO
    };
private:
    CButton wnd_newscene, wnd_newvideo, wnd_newaudio, wnd_record;
    CListBox wnd_scene_list, wnd_video_list, wnd_audio_list;

    void add_new_item(new_item_t);
public:
    enum {IDD = IDD_FORMVIEW};

    gui_frame& wnd_parent;

    explicit gui_maindlg(gui_frame&);
    BOOL PreTranslateMessage(MSG* pMsg);

    // command handlers handle events from the sub windows
    BEGIN_MSG_MAP(gui_maindlg)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_HANDLER(IDC_NEWSCENE, BN_CLICKED, OnBnClickedNewscene)
        COMMAND_HANDLER(IDC_NEWVIDEO, BN_CLICKED, OnBnClickedNewvideo)
        COMMAND_HANDLER(IDC_NEWAUDIO, BN_CLICKED, OnBnClickedNewaudio)
        COMMAND_HANDLER(IDC_BUTTON_RECORD, BN_CLICKED, OnBnClickedButtonRecord)
        COMMAND_HANDLER(IDC_LIST_SCENES, LBN_SELCHANGE, OnLbnSelchangeListScenes)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedNewscene(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedNewvideo(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedNewaudio(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedButtonRecord(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnLbnSelchangeListScenes(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnDestroy(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
};