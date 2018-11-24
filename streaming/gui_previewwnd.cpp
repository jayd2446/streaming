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
    if(DragDetect(*this, point))
    {
        // allows this hwnd to receive mouse events outside the client area
        this->SetCapture();

        this->last_pos = point;
        this->dragging = true;
    }
}

void gui_previewwnd::OnLButtonUp(UINT /*nFlags*/, CPoint /*point*/)
{
    if(this->dragging)
        ReleaseCapture();

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

        const D2D1_RECT_F&& preview_rect = 
            this->ctrl_pipeline->get_preview_window()->get_preview_rect();

        // do not allow dragging if the preview rect has an invalid size
        if(preview_rect.left >= preview_rect.right || preview_rect.top >= preview_rect.bottom)
            return;

        stream_videoprocessor2_controller::params_t params;
        size_box->get_params(params);
        params.dest_m = params.dest_m * D2D1::Matrix3x2F::Translation(
            (FLOAT)move.x * (FLOAT)transform_videoprocessor2::canvas_width / 
            (preview_rect.right - preview_rect.left),
            (FLOAT)move.y * (FLOAT)transform_videoprocessor2::canvas_height / 
            (preview_rect.bottom - preview_rect.top));

        size_box->set_params(params);
    }
}
