#pragma once

#include "wtl.h"
#include "gui_maindlg.h"
#include "gui_preview.h"
#include "control_pipeline.h"
#include <shellapi.h>
#include <atlsplit.h>

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
public:
    DECLARE_FRAME_WND_CLASS(L"streaming", IDR_MAINFRAME);

    gui_maindlg wnd_maindlg;
    gui_preview wnd_preview;
    control_pipeline ctrl_pipeline;

    gui_frame();

    BOOL PreTranslateMessage(MSG* pMsg);
    BOOL OnIdle();

    BEGIN_MSG_MAP(gui_frame)
        MSG_WM_CREATE(OnCreate)
        CHAIN_MSG_MAP(CFrameWindowImpl<gui_frame>)
        COMMAND_ID_HANDLER(ID_ABOUT, OnAbout)
    END_MSG_MAP()

    int OnCreate(LPCREATESTRUCT);
    LRESULT OnAbout(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
};