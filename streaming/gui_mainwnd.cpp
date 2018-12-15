#include "gui_mainwnd.h"

extern CAppModule module_;

#undef max

gui_controlwnd::gui_controlwnd(const control_pipeline2_t& ctrl_pipeline) :
    ctrl_pipeline(ctrl_pipeline),
    dlg_scenes(dlg_sources, ctrl_pipeline),
    dlg_sources(dlg_scenes, ctrl_pipeline),
    dlg_controls(ctrl_pipeline),
    dpi_changed(false)
{
}

BOOL gui_controlwnd::PreTranslateMessage(MSG* pMsg)
{
    return this->IsDialogMessageW(pMsg);
}

int gui_controlwnd::OnCreate(LPCREATESTRUCT)
{
    CMessageLoop* loop = module_.GetMessageLoop();
    loop->AddMessageFilter(this);

    this->dlg_scenes.Create(*this);
    this->dlg_sources.Create(*this);
    this->dlg_controls.Create(*this);

    this->dlg_scenes.ShowWindow(SW_SHOW);
    this->dlg_sources.ShowWindow(SW_SHOW);
    this->dlg_controls.ShowWindow(SW_SHOW);

    return 0;
}

void gui_controlwnd::OnDestroy()
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

    this->ctrl_pipeline->shutdown();

    CMessageLoop* loop = module_.GetMessageLoop();
    loop->RemoveMessageFilter(this);
    this->SetMsgHandled(FALSE);
}

void gui_controlwnd::OnSize(UINT /*nType*/, CSize size)
{
    if(this->dpi_changed)
    {
        this->dpi_changed = false;
    }

    RECT rc = {0};
    rc.bottom = size.cy;

    rc.right = (size.cx - 110) / 2;
    this->dlg_scenes.SetWindowPos(NULL, &rc, SWP_NOACTIVATE | SWP_NOZORDER);

    rc.left = rc.right;
    rc.right += rc.right;
    this->dlg_sources.SetWindowPos(NULL, &rc, SWP_NOACTIVATE | SWP_NOZORDER);

    rc.left = rc.right;
    rc.right = size.cx;
    this->dlg_controls.SetWindowPos(NULL, &rc, SWP_NOACTIVATE | SWP_NOZORDER);
}

LRESULT gui_controlwnd::OnDpiChanged(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    this->dpi_changed = true;
    return 0;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


gui_mainwnd::gui_mainwnd() : 
    ctrl_pipeline(new control_pipeline2),
    wnd_control(this->ctrl_pipeline),
    wnd_preview(wnd_control.dlg_sources, this->ctrl_pipeline),
    // use this class' messagemap(=this) and use map section 1
    wnd_statusbar(this, 1),
    was_minimized(FALSE)
{
    // this failed previously because for some reason
    // the debug version of this exe had the dpi setting overridden in the
    // compatibility section of the exe;
    // https://stackoverflow.com/questions/46651074/wm-dpichanged-not-received-when-scaling-performed-by-application
    // enable setprocessdpiawarenesscontext to allow duplicateoutput1 to succeed
    BOOL success = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    assert_(success);
    success;

    // TODO: wtl is not dpi aware
}

void gui_mainwnd::set_statusbar_parts(CSize size)
{
    int pos[3];
    const int grip_width = ::GetSystemMetrics(SM_CXVSCROLL);
    const int part_width = 200;
    pos[0] = size.cx - part_width * 2 - grip_width;
    pos[1] = size.cx - part_width - grip_width;
    pos[2] = size.cx/* - grip_width*/;

    this->wnd_statusbar.SetParts(ARRAYSIZE(pos), pos);
    this->wnd_statusbar.SetText(1, L"REC: 00:00:00");
    this->wnd_statusbar.SetText(2, L"CPU: 0.0%, 60.00 fps");
}

BOOL gui_mainwnd::PreTranslateMessage(MSG* pMsg)
{
    return CFrameWindowImpl<gui_mainwnd>::PreTranslateMessage(pMsg);
}

BOOL gui_mainwnd::OnIdle()
{
    return FALSE;
}

int gui_mainwnd::OnCreate(LPCREATESTRUCT /*createstruct*/)
{
    CMessageLoop* loop = module_.GetMessageLoop();
    loop->AddMessageFilter(this);
    loop->AddIdleHandler(this);
    
    // create status bar
    RECT rc;
    this->GetClientRect(&rc);
    this->CreateSimpleStatusBar(L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP | CCS_NODIVIDER);
    //this->m_hWndStatusBar = CreateWindowEx(
    //    /*WS_EX_LAYOUTRTL*/0, STATUSCLASSNAME, NULL, SBARS_SIZEGRIP | WS_CHILD | WS_VISIBLE | CCS_NODIVIDER,
    //    0, 0, 0, 0, *this, (HMENU)ATL_IDW_STATUS_BAR, createstruct->hInstance, NULL);
    this->wnd_statusbar.SubclassWindow(this->m_hWndStatusBar);
    this->set_statusbar_parts(CSize(rc.right, rc.bottom));

    // create splitter
    this->wnd_splitter.Create(
        *this, rcDefault, NULL,
        WS_CHILD | WS_VISIBLE /*| WS_CLIPSIBLINGS | WS_CLIPCHILDREN*/,
        WS_EX_CLIENTEDGE);
    this->m_hWndClient = this->wnd_splitter;
    this->UpdateLayout();

    this->wnd_splitter.SetSplitterExtendedStyle(SPLIT_BOTTOMALIGNED);
    this->wnd_splitter.SetOrientation(false);
    this->wnd_splitter.SetSplitterPos(rc.bottom - 200);

    // create preview window
    this->wnd_preview.Create(
        this->wnd_splitter, rcDefault, L"Preview", 
        WS_CHILD /*| WS_CLIPSIBLINGS | WS_CLIPCHILDREN*/);

    // create control window
    this->wnd_control.Create(
        this->wnd_splitter, rcDefault, L"Controls",
        WS_CHILD /*| WS_CLIPSIBLINGS | WS_CLIPCHILDREN*/);

    // set the splitter panes
    this->wnd_splitter.SetSplitterPanes(this->wnd_preview, this->wnd_control);

    // show the windows
    this->wnd_preview.ShowWindow(SW_SHOW);
    this->wnd_control.ShowWindow(SW_SHOW);

    // set the preview window
    this->ctrl_pipeline->set_preview_window(this->wnd_preview);

    return 0;
}

void gui_mainwnd::OnDestroy()
{
    CMessageLoop* loop = module_.GetMessageLoop();
    loop->RemoveIdleHandler(this);
    loop->RemoveMessageFilter(this);
    this->SetMsgHandled(FALSE);
}

void gui_mainwnd::OnSetFocus(CWindow /*old*/)
{
    if(this->wnd_control)
        this->wnd_control.SetFocus();
}

void gui_mainwnd::OnActivate(UINT nState, BOOL bMinimized, CWindow /*wndOther*/)
{
    if(bMinimized != this->was_minimized)
    {
        this->was_minimized = bMinimized;

        control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
        if(this->ctrl_pipeline->get_preview_window())
            this->ctrl_pipeline->get_preview_window()->set_state(!bMinimized);
    }

    if(!bMinimized)
    {
        if(nState == WA_INACTIVE)
        {
            HWND hwnd_focus = ::GetFocus();
            if(hwnd_focus && this->IsChild(hwnd_focus))
                this->last_focus = hwnd_focus;
        }
        else
        {
            if(this->last_focus)
                this->last_focus.SetFocus();
            else
                this->SetMsgHandled(FALSE);
        }
    }
    else
    {


        this->SetMsgHandled(FALSE);
    }
}

void gui_mainwnd::OnStatusBarSize(UINT /*nType*/, CSize size)
{
    if(size.cx != 0 || size.cy != 0)
        this->set_statusbar_parts(size);

    // let the superclass handle the rest of sizing
    this->SetMsgHandled(FALSE);
}

LRESULT gui_mainwnd::OnStatusBarSimple(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    // override the default behaviour of sb_simple call which will hide all but the first
    // part of the status bar;
    // clicking on the system menu requests the simple mode for the status bar

    // ret val is not used
    this->SetMsgHandled(TRUE);
    return FALSE;
}

LRESULT gui_mainwnd::OnDpiChanged(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/)
{
    // https://github.com/Microsoft/Windows-classic-samples/blob/master/Samples/DPIAwarenessPerWindow/cpp/DpiAwarenessContext.cpp#L280
    const UINT dpi = HIWORD(wParam);
    LPCRECT new_scale = (LPCRECT)lParam;

    dpi;
    this->SetWindowPos(NULL, new_scale, SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
}

LRESULT gui_mainwnd::OnAbout(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    this->MessageBoxW(L"scuffed gui ver. 0.1", L"About", MB_ICONINFORMATION);
    return 0;
}

LRESULT gui_mainwnd::OnDebug(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    {
        control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
        if(this->ctrl_pipeline->root_scene.video_controls.size() < 2)
            return 0;
    }

    for(int i = 0; i < 19; i++)
    {
        control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
        this->wnd_control.dlg_scenes.wnd_scenelist.SetCurSel(i % 2);
        this->ctrl_pipeline->root_scene.switch_scene(true, i % 2);
        this->wnd_control.dlg_sources.set_source_tree(
            this->ctrl_pipeline->root_scene.get_active_scene());
    }

    return 0;
}
