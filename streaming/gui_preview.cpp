#include "gui_preview.h"
#include "gui_frame.h"

gui_preview::gui_preview(gui_frame& wnd_parent) : wnd_parent(wnd_parent)
{
}

LRESULT gui_preview::OnSize(UINT /*nType*/, CSize /*Extent*/)
{
    if(this->wnd_parent.ctrl_pipeline.is_running())
    {
        this->wnd_parent.ctrl_pipeline.update_preview_size();
        this->wnd_parent.wnd_maindlg.RedrawWindow();
    }
    return 0;
}