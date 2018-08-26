#pragma once

#include "wtl.h"
#include "gui_maindlg.h"
#include "gui_preview.h"
#include "control_pipeline.h"
#include <shellapi.h>
#include <atlsplit.h>
#include <atlctrlx.h>

/*
In WTL, the top-level window can be a dialog box itself, or can be a mainframe
containing a child window known as a view, based on a dialog (resource template) or a
single control. When the top-level window is not a dialog, it usually contains a
command bar (menubar) and toolbar, whose entries when selected often result in dialog
boxes being displayed. 
*/

class gui_frame : 
    public CFrameWindowImpl<gui_frame>,
    public CMessageFilter,
    public CIdleHandler
{
private:
    CSplitterWindow wnd_splitter;
    CPaneContainer wnd_panecontainer;
    CAppModule& module;
public:
    DECLARE_FRAME_WND_CLASS(L"streaming", IDR_MAINFRAME);

    gui_maindlg wnd_maindlg;
    gui_preview wnd_preview;
    control_pipeline ctrl_pipeline;

    explicit gui_frame(CAppModule&);

    BOOL PreTranslateMessage(MSG* pMsg);
    BOOL OnIdle();

    BEGIN_MSG_MAP(gui_frame)
        MSG_WM_DESTROY(OnDestroy)
        MSG_WM_CREATE(OnCreate)
        MSG_WM_SETFOCUS(OnSetFocus)
        MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
        COMMAND_ID_HANDLER(ID_ABOUT, OnAbout)
        CHAIN_MSG_MAP(CFrameWindowImpl<gui_frame>)
    END_MSG_MAP()

    void OnDestroy();
    int OnCreate(LPCREATESTRUCT);
    void OnSetFocus(CWindow /*old*/);
    LRESULT OnDpiChanged(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnAbout(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
};