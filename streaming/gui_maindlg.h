#pragma once

#include "wtl.h"
#include "gui_newdlg.h"

/*
The ATL Object Wizard does not provide wizard support to use CWindowImpl (but it
does for dialog boxes). It is easy enough to use it manually. You must create a C++
class which derives from CWindowImpl, write a constructor which calls Create and
a destructor which calls DestroyWindow, and add a message map which implements
ProcessWindowMessage. 
*/

class gui_frame;

class gui_maindlg : 
    public CDialogImpl<gui_maindlg>,
    public CDialogResize<gui_maindlg>
{
private:
    CButton wnd_newscene, wnd_newvideo, wnd_newaudio, wnd_record;
    CListBox wnd_scene_list, wnd_video_list, wnd_audio_list;

    void add_new_item(gui_newdlg::new_item_t);
public:
    enum {IDD = IDD_FORMVIEW};

    gui_frame& wnd_parent;

    explicit gui_maindlg(gui_frame&);
    BOOL PreTranslateMessage(MSG* pMsg);

    // command handlers handle events from the child windows
    BEGIN_MSG_MAP(gui_maindlg)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_HANDLER(IDC_NEWSCENE, BN_CLICKED, OnBnClickedNewscene)
        COMMAND_HANDLER(IDC_NEWVIDEO, BN_CLICKED, OnBnClickedNewvideo)
        COMMAND_HANDLER(IDC_NEWAUDIO, BN_CLICKED, OnBnClickedNewaudio)
        COMMAND_HANDLER(IDC_BUTTON_RECORD, BN_CLICKED, OnBnClickedButtonRecord)
        COMMAND_HANDLER(IDC_LIST_SCENES, LBN_SELCHANGE, OnLbnSelchangeListScenes)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        CHAIN_MSG_MAP(CDialogResize<gui_maindlg>)
    END_MSG_MAP()

    BEGIN_DLGRESIZE_MAP(gui_maindlg)
        /*BEGIN_DLGRESIZE_GROUP()*/
            DLGRESIZE_CONTROL(IDC_NEWSCENE, DLSZ_SIZE_X)
            DLGRESIZE_CONTROL(IDC_NEWVIDEO, DLSZ_SIZE_X)
            DLGRESIZE_CONTROL(IDC_NEWAUDIO, DLSZ_SIZE_X)
            DLGRESIZE_CONTROL(IDC_BUTTON_RECORD, DLSZ_SIZE_X)
        /*END_DLGRESIZE_GROUP()*/
    END_DLGRESIZE_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedNewscene(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedNewvideo(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedNewaudio(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedButtonRecord(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnLbnSelchangeListScenes(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnDestroy(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
};