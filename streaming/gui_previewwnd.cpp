#include "gui_previewwnd.h"
#define _USE_MATH_DEFINES
#include <math.h>
#include <iostream>

#define DRAG_RADIUS_OFFSET 2.f

FLOAT _rot = -15.f;

gui_previewwnd::gui_previewwnd(const control_pipeline2_t& ctrl_pipeline) : 
    ctrl_pipeline(ctrl_pipeline),
    dragging(false), scaling(false), moving(false),
    scale_flags(0)
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

void gui_previewwnd::OnRButtonDown(UINT /*nFlags*/, CPoint /*point*/)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
    if(!this->ctrl_pipeline->get_preview_window())
        return;
    if(this->ctrl_pipeline->selected_items.empty())
        return;
    control_video2* video_control = dynamic_cast<control_video2*>(
        this->ctrl_pipeline->selected_items[0].get());
    if(!video_control)
        return;

    /*_rot += -10.f;
    video_control->apply_transformation(true);*/

    video_control->rotate(-10.f);
    video_control->apply_transformation();

    //static FLOAT y = 30.f;

    ///*control_video::video_params_t params = video_control->get_video_params(true);
    //params.rotation += -10.f;
    //video_control->apply_video_params(params);*/

    //video_control->scale(this->ctrl_pipeline->get_preview_window(),
    //    (FLOAT)this->ctrl_pipeline->get_preview_window()->get_preview_rect().left, (FLOAT)y,
    //    (FLOAT)0.f, (FLOAT)0.f, control_video2::SCALE_TOP | control_video2::SCALE_LEFT | control_video2::ABSOLUTE_MODE);

    //y += 10.f;
}

void gui_previewwnd::OnLButtonDown(UINT /*nFlags*/, CPoint point)
{
    if(DragDetect(*this, point) /*|| this->scale_flags*/)
    {
        // TODO: the dragging type should be chosen here

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

    this->dragging = this->scaling = this->moving = false;
    this->scale_flags = 0;
}

void gui_previewwnd::OnMouseMove(UINT /*nFlags*/, CPoint point)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
    if(!this->ctrl_pipeline->get_preview_window())
        return;
    if(this->ctrl_pipeline->selected_items.empty())
        return;
    control_video2* video_control = dynamic_cast<control_video2*>(
        this->ctrl_pipeline->selected_items[0].get());
    if(!video_control)
        return;

    /*control_video2::video_params_t video_params = video_control->get_video_params(true);*/
    D2D1_RECT_F preview_rect = this->ctrl_pipeline->get_preview_window()->get_preview_rect();
    // do not allow dragging if the preview rect has an invalid size
    if(preview_rect.left >= preview_rect.right || preview_rect.top >= preview_rect.bottom)
        return;

    const FLOAT size_point_radius =
        this->ctrl_pipeline->get_preview_window()->get_size_point_radius() +
        DRAG_RADIUS_OFFSET;
    D2D1_POINT_2F sizing_points[8];
    video_control->get_sizing_points(this->ctrl_pipeline->get_preview_window(),
        sizing_points, ARRAYSIZE(sizing_points));
    const D2D1_POINT_2F pointer_pos = D2D1::Point2F((FLOAT)point.x, (FLOAT)point.y);

    if(this->moving || this->scaling)
        goto dragging;

    if(!this->dragging)
        this->scale_flags = 0;

    // TODO: previewwnd should probably handle the drawing of sizing points

    bool left_scaled = false, top_scaled = false, right_scaled = false, bottom_scaled = false;
    if(pointer_pos.x >= (sizing_points[0].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[0].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[0].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[0].y + size_point_radius))
    {
        left_scaled = top_scaled = true;
        this->scale_flags |= control_video2::SCALE_LEFT | control_video2::SCALE_TOP;
    }
    if(pointer_pos.x >= (sizing_points[1].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[1].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[1].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[1].y + size_point_radius) &&
        !left_scaled && !bottom_scaled)
    {
        right_scaled = top_scaled = true;
        this->scale_flags |= control_video2::SCALE_RIGHT | control_video2::SCALE_TOP;
    }
    if(pointer_pos.x >= (sizing_points[2].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[2].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[2].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[2].y + size_point_radius) &&
        !right_scaled && !top_scaled)
    {
        left_scaled = bottom_scaled = true;
        this->scale_flags |= control_video2::SCALE_LEFT | control_video2::SCALE_BOTTOM;
    }
    if(pointer_pos.x >= (sizing_points[3].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[3].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[3].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[3].y + size_point_radius) &&
        !left_scaled && !top_scaled)
        this->scale_flags |= control_video2::SCALE_RIGHT | control_video2::SCALE_BOTTOM;

    /////////////////////////////////////////////////////////////////////////////////////////

    left_scaled = false, top_scaled = false, right_scaled = false, bottom_scaled = false;
    if(pointer_pos.x >= (sizing_points[4].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[4].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[4].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[4].y + size_point_radius))
    {
        left_scaled = top_scaled = true;
        this->scale_flags |= control_video2::SCALE_TOP;
    }
    if(pointer_pos.x >= (sizing_points[5].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[5].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[5].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[5].y + size_point_radius) &&
        !left_scaled && !bottom_scaled)
    {
        right_scaled = top_scaled = true;
        this->scale_flags |= control_video2::SCALE_RIGHT;
    }
    if(pointer_pos.x >= (sizing_points[6].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[6].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[6].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[6].y + size_point_radius) &&
        !right_scaled && !top_scaled)
    {
        left_scaled = bottom_scaled = true;
        this->scale_flags |= control_video2::SCALE_LEFT;
    }
    if(pointer_pos.x >= (sizing_points[7].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[7].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[7].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[7].y + size_point_radius) &&
        !left_scaled && !top_scaled)
        this->scale_flags |= control_video2::SCALE_BOTTOM;

    video_control->highlight_sizing_points(this->scale_flags);

    if(this->dragging)
    {
    dragging:
        D2D1_POINT_2F pos = video_control->client_to_canvas(
            this->ctrl_pipeline->get_preview_window(), point.x, point.y);
        const D2D1_POINT_2F old_pos = video_control->client_to_canvas(
                this->ctrl_pipeline->get_preview_window(), this->last_pos.x, this->last_pos.y);
        D2D1_POINT_2F move = pos;
        move.x -= old_pos.x;
        move.y -= old_pos.y;
        CPoint move_client = point;
        move_client.x -= this->last_pos.x;
        move_client.y -= this->last_pos.y;

        /*this->last_pos = point;*/

        if((this->scale_flags || this->scaling) && !this->moving)
        {
            /*this->last_pos = point;

            this->scaling = true;
            video_control->scale(pos.x, pos.y, pos.x, pos.y, 
                this->scale_flags | control_video2::ABSOLUTE_MODE);*/

            this->scaling = true;

            video_control->scale(pos.x, pos.y, pos.x, pos.y,
                this->scale_flags | control_video2::ABSOLUTE_MODE);

            int scale_type = ~this->scale_flags;
            const D2D1_POINT_2F clamping_vector = video_control->get_clamping_vector(
                this->ctrl_pipeline->get_preview_window(), D2D1::Point2F(), scale_type);

            bool allow_clamping_x, allow_clamping_y;

            // TODO: rotated clamping fails because logic allows sliding
            // TODO: left side of clamping fails

            if(allow_clamping_x = video_control->allow_clamping(move_client.x))
            {
                /*move.x += clamping_vector.x;*/
                if(!clamping_vector.x)
                    this->last_pos.x = point.x;
            }
            else
                this->last_pos.x = point.x;
            if(allow_clamping_y = video_control->allow_clamping(move_client.y))
            {
                /*move.y += clamping_vector.y;*/
                if(!clamping_vector.y)
                    this->last_pos.y = point.y;
            }
            else
                this->last_pos.y = point.y;

            D2D1_POINT_2F dmove = pos;
            bool do_clamp = false;
            if(clamping_vector.x && allow_clamping_x)
                dmove.x += clamping_vector.x, do_clamp = true;
            if(clamping_vector.y && allow_clamping_y)
                dmove.y += clamping_vector.y, do_clamp = true;

            if(do_clamp)
                video_control->scale(dmove.x, dmove.y, dmove.x, dmove.y, 
                    this->scale_flags | control_video2::ABSOLUTE_MODE);

            video_control->apply_transformation();
        }
        else
        {
            this->moving = true;

            video_control->move(move.x, move.y);

            int scale_type = 0;
            const D2D1_POINT_2F clamping_vector = video_control->get_clamping_vector(
                this->ctrl_pipeline->get_preview_window(), D2D1::Point2F(), scale_type);

            bool allow_clamping_x, allow_clamping_y;

            if(allow_clamping_x = video_control->allow_clamping(move_client.x))
            {
                /*move.x += clamping_vector.x;*/
                if(!clamping_vector.x)
                    this->last_pos.x = point.x;
            }
            else
                this->last_pos.x = point.x;
            if(allow_clamping_y = video_control->allow_clamping(move_client.y))
            {
                /*move.y += clamping_vector.y;*/
                if(!clamping_vector.y)
                    this->last_pos.y = point.y;
            }
            else
                this->last_pos.y = point.y;

            D2D1_POINT_2F dmove = D2D1::Point2F();
            if(clamping_vector.x && allow_clamping_x)
                dmove.x = clamping_vector.x;
            if(clamping_vector.y && allow_clamping_y)
                dmove.y = clamping_vector.y;

            if(dmove.x || dmove.y)
                video_control->move(dmove.x, dmove.y);
            /*video_control->move(move.x, move.y);*/

            video_control->apply_transformation();
        }
    }
}
