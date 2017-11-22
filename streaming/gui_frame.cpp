#include "gui_frame.h"

extern CAppModule _Module;

gui_frame::gui_frame() : wnd_maindlg(*this), wnd_preview(*this)
{
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

int gui_frame::OnCreate(LPCREATESTRUCT)
{
    CMessageLoop* loop = _Module.GetMessageLoop();
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

LRESULT gui_frame::OnAbout(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    this->MessageBoxW(L"scuffed gui ver. 0.0", L"About", MB_ICONINFORMATION);
    return 0;
}
