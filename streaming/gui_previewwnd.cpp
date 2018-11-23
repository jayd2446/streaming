#include "gui_previewwnd.h"
#include <iostream>

gui_previewwnd::gui_previewwnd(const control_pipeline2_t& ctrl_pipeline) : 
    ctrl_pipeline(ctrl_pipeline),
    dragging(false)
{
}

LRESULT gui_previewwnd::OnSize(UINT /*nType*/, CSize /*Extent*/)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
    if(this->ctrl_pipeline->is_running())
    {
        this->ctrl_pipeline->update_preview_size();
        /*this->wnd_parent.wnd_maindlg.RedrawWindow();*/
    }
    return 0;
}

void gui_previewwnd::OnLButtonDown(UINT /*nFlags*/, CPoint point)
{
    this->last_pos = point;
    this->dragging = true;
}

void gui_previewwnd::OnLButtonUp(UINT /*nFlags*/, CPoint /*point*/)
{
    this->dragging = false;
}

void gui_previewwnd::OnMouseMove(UINT /*nFlags*/, CPoint point)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

    if(!this->ctrl_pipeline->get_preview_window())
        return;

    if(this->dragging)
    {
        stream_videoprocessor2_controller_t&& size_box =
            this->ctrl_pipeline->get_preview_window()->get_size_box();
        if(!size_box)
            return;

        CPoint move = point;
        move -= this->last_pos;
        this->last_pos = point;

        stream_videoprocessor2_controller::params_t params;
        size_box->get_params(params);
        params.dest_m = params.dest_m * D2D1::Matrix3x2F::Translation(move.x, move.y);
        /*params.dest_rect.left += move.x;
        params.dest_rect.top += move.y;
        params.dest_rect.right += move.x;
        params.dest_rect.bottom += move.y;*/
        size_box->set_params(params);
    }
}
