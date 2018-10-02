#pragma once

#include "wtl.h"
#include "gui_previewwnd.h"
#include "gui_dlgs.h"
#include "control_pipeline2.h"
#include <atlsplit.h>

class gui_mainwnd;

// hosts the dialogs
class gui_controlwnd : 
    public CWindowImpl<gui_controlwnd>,
    public CMessageFilter
{
private:
    control_pipeline2_t ctrl_pipeline;
    gui_scenedlg dlg_scenes;
    gui_sourcedlg dlg_sources;
    gui_controldlg dlg_controls;

    bool dpi_changed;
public:
    DECLARE_WND_CLASS(L"control")

    explicit gui_controlwnd(const control_pipeline2_t&);

    BOOL PreTranslateMessage(MSG* pMsg);

    BEGIN_MSG_MAP(gui_controlwnd)
        MSG_WM_CREATE(OnCreate)
        MSG_WM_DESTROY(OnDestroy)
        MSG_WM_SIZE(OnSize)
        MESSAGE_HANDLER(WM_DPICHANGED_BEFOREPARENT, OnDpiChanged)
    END_MSG_MAP()

    int OnCreate(LPCREATESTRUCT);
    void OnDestroy();
    void OnSize(UINT /*nType*/, CSize size);
    LRESULT OnDpiChanged(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
};

// hosts the preview and control window using a splitter window
class gui_mainwnd :
    public CFrameWindowImpl<gui_mainwnd>,
    public CMessageFilter,
    public CIdleHandler
{
public:
private:
    control_pipeline2_t ctrl_pipeline;

    BOOL was_minimized;
    CSplitterWindow wnd_splitter;
    gui_previewwnd wnd_preview;
    gui_controlwnd wnd_control;
    CWindow last_focus;
    CContainedWindowT<CStatusBarCtrl> wnd_statusbar;

    void set_statusbar_parts(CSize);
public:
    DECLARE_FRAME_WND_CLASS(L"streaming", IDR_MAINFRAME)

    gui_mainwnd();

    BOOL PreTranslateMessage(MSG* pMsg);
    BOOL OnIdle();
    BEGIN_MSG_MAP(gui_mainwnd)
        MSG_WM_CREATE(OnCreate)
        MSG_WM_DESTROY(OnDestroy)
        MSG_WM_SETFOCUS(OnSetFocus)
        MSG_WM_ACTIVATE(OnActivate)
        MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
        COMMAND_ID_HANDLER(ID_ABOUT, OnAbout)

        CHAIN_MSG_MAP(CFrameWindowImpl<gui_mainwnd>)

        // alt msg map must be at the end
        ALT_MSG_MAP(1) // 1 is the id for the ccontainedwindow; the begin_msg_map is identified by 0
            MSG_WM_SIZE(OnStatusBarSize)
            MESSAGE_HANDLER(SB_SIMPLE, OnStatusBarSimple)
    END_MSG_MAP()

    int OnCreate(LPCREATESTRUCT);
    void OnDestroy();
    void OnSetFocus(CWindow /*old*/);
    void OnActivate(UINT /*nState*/, BOOL /*bMinimized*/, CWindow /*wndOther*/);
    void OnStatusBarSize(UINT /*nType*/, CSize size);
    LRESULT OnStatusBarSimple(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnDpiChanged(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
    LRESULT OnAbout(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
};