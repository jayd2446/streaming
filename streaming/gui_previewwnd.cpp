#include "gui_previewwnd.h"

gui_previewwnd::gui_previewwnd(control_pipeline& ctrl_pipeline) : ctrl_pipeline(ctrl_pipeline)
{
}

LRESULT gui_previewwnd::OnSize(UINT /*nType*/, CSize /*Extent*/)
{
    if(this->ctrl_pipeline.is_running())
    {
        this->ctrl_pipeline.update_preview_size();
        /*this->wnd_parent.wnd_maindlg.RedrawWindow();*/
    }
    return 0;
}