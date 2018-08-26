#include "gui_previewwnd.h"

gui_previewwnd::gui_previewwnd(const control_pipeline_t& ctrl_pipeline) : ctrl_pipeline(ctrl_pipeline)
{
}

LRESULT gui_previewwnd::OnSize(UINT /*nType*/, CSize /*Extent*/)
{
    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);
    if(this->ctrl_pipeline->is_running())
    {
        this->ctrl_pipeline->update_preview_size();
        /*this->wnd_parent.wnd_maindlg.RedrawWindow();*/
    }
    return 0;
}