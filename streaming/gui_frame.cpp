#include "gui_frame.h"

// windows 10 specific
DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
typedef BOOL (WINAPI *PFNSETPROCESSDPIAWARENESSCONTEXT)(DPI_AWARENESS_CONTEXT value);
#define DPI_AWARENESS_CONTEXT_UNAWARE              ((DPI_AWARENESS_CONTEXT)-1)
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE         ((DPI_AWARENESS_CONTEXT)-2)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE    ((DPI_AWARENESS_CONTEXT)-3)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)

gui_frame::gui_frame(CAppModule& module) : module(module), wnd_maindlg(*this), wnd_preview(*this)
{
    PFNSETPROCESSDPIAWARENESSCONTEXT SetProcessDpiAwarenessContext  = (PFNSETPROCESSDPIAWARENESSCONTEXT)
        GetProcAddress(GetModuleHandle(L"user32.dll"), "SetProcessDpiAwarenessContext");
    
    if(SetProcessDpiAwarenessContext)
    {
        // this failed previously because for some reason
        // the debug version of this exe had the dpi setting overridden in the
        // compatibility section of the exe;
        // https://stackoverflow.com/questions/46651074/wm-dpichanged-not-received-when-scaling-performed-by-application
        BOOL success = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        assert_(success);
    }
}

BOOL gui_frame::PreTranslateMessage(MSG* pMsg)
{
    if(this->wnd_maindlg.PreTranslateMessage(pMsg))
        return TRUE;
    return CFrameWindowImpl<gui_frame>::PreTranslateMessage(pMsg);
}

BOOL gui_frame::OnIdle()
{
    return FALSE;
}

void gui_frame::OnDestroy()
{
    CMessageLoop* loop = this->module.GetMessageLoop();
    loop->RemoveIdleHandler(this);
    loop->RemoveMessageFilter(this);
    this->SetMsgHandled(FALSE);
}

int gui_frame::OnCreate(LPCREATESTRUCT)
{
    CMessageLoop* loop = this->module.GetMessageLoop();
    loop->AddMessageFilter(this);
    loop->AddIdleHandler(this);

    this->wnd_splitter.Create(*this, rcDefault, NULL, 0, WS_EX_CLIENTEDGE);
    this->m_hWndClient = this->wnd_splitter;
    this->UpdateLayout();

    this->wnd_splitter.SetOrientation(false);
    this->wnd_splitter.SetSplitterPos(480);

    this->wnd_preview.Create(this->wnd_splitter);

    this->wnd_maindlg.Create(this->wnd_splitter);
    this->wnd_splitter.SetSplitterPanes(this->wnd_preview, this->wnd_maindlg);
    this->wnd_maindlg.ShowWindow(SW_SHOW);
    this->wnd_preview.ShowWindow(SW_SHOW);

    this->ctrl_pipeline.initialize(this->wnd_preview);

    return 0;
}

void gui_frame::OnSetFocus(CWindow /*old*/)
{
    this->wnd_maindlg.SetFocus();
}

LRESULT gui_frame::OnDpiChanged(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/)
{
    // https://github.com/Microsoft/Windows-classic-samples/blob/master/Samples/DPIAwarenessPerWindow/cpp/DpiAwarenessContext.cpp#L280
    const UINT dpi = HIWORD(wParam);
    LPCRECT new_scale = (LPCRECT)lParam;

    dpi;
    this->SetWindowPos(NULL, new_scale, SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
}

LRESULT gui_frame::OnAbout(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    this->MessageBoxW(L"scuffed gui ver. 0.0", L"About", MB_ICONINFORMATION);
    return 0;
}
