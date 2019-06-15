#include "gui_previewwnd.h"
#include "gui_dlgs.h"
#define _USE_MATH_DEFINES
#include <math.h>
#include <iostream>

#pragma warning(push)
#pragma warning(disable: 4706) // assignment within conditional expression

#define DRAG_RADIUS_OFFSET 2.f

gui_previewwnd::gui_previewwnd(const control_pipeline2_t& ctrl_pipeline) :
    ctrl_pipeline(ctrl_pipeline),
    dragging(false), scaling(false), moving(false),
    scale_flags(0),
    sizing_point(0)
{
}

bool gui_previewwnd::select_item(CPoint point, bool& first_selection, bool select_next)
{
    first_selection = false;

    // select item
    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);
    if(!this->ctrl_pipeline->get_preview_window())
        return false;

    control_scene* scene = this->ctrl_pipeline->root_scene->get_selected_scene();
    if(scene->get_video_controls().empty())
        return false;
    auto start_it = scene->get_video_controls().begin();
    if(!this->ctrl_pipeline->get_selected_controls().empty())
    {
        // it is assumed that the selected item is contained in the active scene
        control_video* video_control = dynamic_cast<control_video*>(
            this->ctrl_pipeline->get_selected_controls()[0]);
        if(video_control)
        {
            bool is_video_control, found;
            auto it = scene->find_control_iterator(video_control->name, is_video_control, found);
            if(is_video_control && found)
            {
                if(select_next)
                    start_it = it + 1;
                else
                    start_it = it;
            }
        }
    }

    bool item_selected = false;
    auto it = start_it;
    do
    {
        if(it == scene->get_video_controls().end())
        {
            it = scene->get_video_controls().begin();
            if(it == start_it)
                break;
        }

        control_video* video_control = dynamic_cast<control_video*>(it->get());
        if(!video_control)
            continue;

        using namespace D2D1;
        Matrix3x2F invert_dest_m = video_control->get_transformation(true);
        const bool inverted = invert_dest_m.Invert(); inverted;

        D2D1_POINT_2F selection_pos = video_control->client_to_canvas(
            this->ctrl_pipeline->get_preview_window(), point.x, point.y);
        selection_pos = selection_pos * invert_dest_m;

        const D2D1_RECT_F rectangle = video_control->get_rectangle(true);
        if(selection_pos.x >= rectangle.left && selection_pos.y >= rectangle.top &&
            selection_pos.x <= rectangle.right && selection_pos.y <= rectangle.bottom)
        {
            // select the item
            first_selection = this->ctrl_pipeline->get_selected_controls().empty();
            this->ctrl_pipeline->set_selected_control(it->get());
            item_selected = true;
            break;
        }

        it++;
    }
    while(it != start_it);

    // unselect all items
    if(!item_selected)
        this->ctrl_pipeline->set_selected_control(NULL, control_pipeline::CLEAR);

    return item_selected;
}

LRESULT gui_previewwnd::OnSize(UINT /*nType*/, CSize /*Extent*/)
{
    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);
    if(this->ctrl_pipeline->get_preview_window())
    {
        this->ctrl_pipeline->get_preview_window()->update_size();
        /*this->wnd_parent.wnd_maindlg.RedrawWindow();*/
    }
    return 0;
}

void gui_previewwnd::OnRButtonDown(UINT /*nFlags*/, CPoint /*point*/)
{
    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);
    if(!this->ctrl_pipeline->get_preview_window())
        return;
    if(this->ctrl_pipeline->get_selected_controls().empty())
        return;
    control_video* video_control = dynamic_cast<control_video*>(
        this->ctrl_pipeline->get_selected_controls()[0]);
    if(!video_control)
        return;

    /*_rot += -10.f;
    video_control->apply_transformation(true);*/

    /*video_control->scale(40.f, 40.f, 
        control_video::SCALE_LEFT | control_video::PRESERVE_ASPECT_RATIO,
        false);*/
    video_control->rotate(-10.f);
    video_control->apply_transformation();

    //static FLOAT y = 30.f;

    ///*control_video::video_params_t params = video_control->get_video_params(true);
    //params.rotation += -10.f;
    //video_control->apply_video_params(params);*/

    //video_control->scale(this->ctrl_pipeline->get_preview_window(),
    //    (FLOAT)this->ctrl_pipeline->get_preview_window()->get_preview_rect().left, (FLOAT)y,
    //    (FLOAT)0.f, (FLOAT)0.f, control_video::SCALE_TOP | control_video::SCALE_LEFT | control_video::ABSOLUTE_MODE);

    //y += 10.f;
}

void gui_previewwnd::OnLButtonDown(UINT /*nFlags*/, CPoint point)
{
    int dragging = 0;
    {
        control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);
        control_video* video_control;
        if(!this->ctrl_pipeline->get_selected_controls().empty() &&
            (video_control = 
                dynamic_cast<control_video*>(this->ctrl_pipeline->get_selected_controls()[0])))
        {
            dragging = video_control->get_highlighted_points();
        }
    }
    bool first_selection = true;
    bool selected = (bool)dragging;
    if(!selected)
        selected = this->select_item(point, first_selection);

    if(DragDetect(*this, point))
    {
        if(!selected)
            return;

        // allows this hwnd to receive mouse events outside the client area
        this->SetCapture();

        this->last_pos = point;
        this->dragging = true;

        // initialize the pos to center vector
        control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);
        if(this->ctrl_pipeline->get_selected_controls().empty())
            return;
        control_video* video_control = dynamic_cast<control_video*>(
            this->ctrl_pipeline->get_selected_controls()[0]);
        if(!video_control)
            return;

        const D2D1_POINT_2F pos = video_control->client_to_canvas(
            this->ctrl_pipeline->get_preview_window(), point.x, point.y);
        this->pos_to_center = video_control->get_center();
        this->pos_to_center.x -= pos.x;
        this->pos_to_center.y -= pos.y;
    }
    else if(!first_selection && selected)
        this->select_item(point, first_selection, true);
}

void gui_previewwnd::OnLButtonUp(UINT /*nFlags*/, CPoint /*point*/)
{
    if(this->dragging)
        ReleaseCapture();

    this->dragging = this->scaling = this->moving = false;
    this->sizing_point = this->scale_flags = 0;
}

void gui_previewwnd::OnMouseMove(UINT /*nFlags*/, CPoint point)
{
    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);
    if(!this->ctrl_pipeline->get_preview_window())
        return;
    if(this->ctrl_pipeline->get_selected_controls().empty())
        return;
    control_video* video_control = dynamic_cast<control_video*>(
        this->ctrl_pipeline->get_selected_controls()[0]);
    if(!video_control)
        return;

    /*control_video::video_params_t video_params = video_control->get_video_params(true);*/
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
        this->sizing_point = 0;
        left_scaled = top_scaled = true;
        this->scale_flags |= control_video::SCALE_LEFT | control_video::SCALE_TOP;
    }
    if(pointer_pos.x >= (sizing_points[1].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[1].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[1].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[1].y + size_point_radius) &&
        !left_scaled && !bottom_scaled)
    {
        this->sizing_point = 1;
        right_scaled = top_scaled = true;
        this->scale_flags |= control_video::SCALE_RIGHT | control_video::SCALE_TOP;
    }
    if(pointer_pos.x >= (sizing_points[2].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[2].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[2].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[2].y + size_point_radius) &&
        !right_scaled && !top_scaled)
    {
        this->sizing_point = 2;
        left_scaled = bottom_scaled = true;
        this->scale_flags |= control_video::SCALE_LEFT | control_video::SCALE_BOTTOM;
    }
    if(pointer_pos.x >= (sizing_points[3].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[3].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[3].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[3].y + size_point_radius) &&
        !left_scaled && !top_scaled)
    {
        this->sizing_point = 3;
        this->scale_flags |= control_video::SCALE_RIGHT | control_video::SCALE_BOTTOM;
    }

    /////////////////////////////////////////////////////////////////////////////////////////

    left_scaled = false, top_scaled = false, right_scaled = false, bottom_scaled = false;
    if(pointer_pos.x >= (sizing_points[4].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[4].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[4].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[4].y + size_point_radius))
    {
        this->sizing_point = 4;
        left_scaled = top_scaled = true;
        this->scale_flags |= control_video::SCALE_TOP;
    }
    if(pointer_pos.x >= (sizing_points[5].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[5].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[5].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[5].y + size_point_radius) &&
        !left_scaled && !bottom_scaled)
    {
        this->sizing_point = 5;
        right_scaled = top_scaled = true;
        this->scale_flags |= control_video::SCALE_RIGHT;
    }
    if(pointer_pos.x >= (sizing_points[6].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[6].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[6].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[6].y + size_point_radius) &&
        !right_scaled && !top_scaled)
    {
        this->sizing_point = 6;
        left_scaled = bottom_scaled = true;
        this->scale_flags |= control_video::SCALE_LEFT;
    }
    if(pointer_pos.x >= (sizing_points[7].x - size_point_radius) &&
        pointer_pos.x <= (sizing_points[7].x + size_point_radius) &&
        pointer_pos.y >= (sizing_points[7].y - size_point_radius) &&
        pointer_pos.y <= (sizing_points[7].y + size_point_radius) &&
        !left_scaled && !top_scaled)
    {
        this->sizing_point = 7;
        this->scale_flags |= control_video::SCALE_BOTTOM;
    }

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

        this->last_pos = point;

        if((this->scale_flags || this->scaling) && !this->moving)
        {
            this->scaling = true;

            video_control->push_matrix();

            // TODO: rotated clamping doesn't always work

            video_control->scale(pos.x, pos.y, this->scale_flags | control_video::ABSOLUTE_MODE);
            bool x_clamped, y_clamped;
            // clamp the max distance
            const D2D1_POINT_2F clamping_vector = video_control->get_clamping_vector(
                this->ctrl_pipeline->get_preview_window(), x_clamped, y_clamped, this->sizing_point);
            if(x_clamped || y_clamped)
                video_control->scale(clamping_vector.x, clamping_vector.y, this->scale_flags, false);

            D2D1_POINT_2F points[8];
            video_control->get_sizing_points(NULL, points, ARRAYSIZE(points));

            video_control->pop_matrix();

            video_control->scale(
                points[this->sizing_point].x, points[this->sizing_point].y, this->scale_flags |
                control_video::ABSOLUTE_MODE | control_video::PRESERVE_ASPECT_RATIO);

            /*video_control->align_source_rect();
            video_control->apply_transformation(false);*/

            video_control->apply_transformation();
        }
        else
        {
            this->moving = true;

            pos.x += this->pos_to_center.x;
            pos.y += this->pos_to_center.y;

            video_control->move(pos.x, pos.y);

            bool x_clamped, y_clamped;
            // clamp the min distance
            const D2D1_POINT_2F clamping_vector = video_control->get_clamping_vector(
                this->ctrl_pipeline->get_preview_window(), x_clamped, y_clamped);

            if(x_clamped || y_clamped)
                video_control->move(clamping_vector.x, clamping_vector.y, false, false);
            video_control->apply_transformation();
        }
    }
}

#pragma warning(pop)