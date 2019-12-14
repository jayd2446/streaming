#include "gui_previewwnd.h"
#include "control_pipeline.h"
#include "gui_dlgs.h"
#define _USE_MATH_DEFINES
#include <math.h>
#include <iostream>

#pragma warning(push)
#pragma warning(disable: 4706) // assignment within conditional expression

#define SIZE_POINT_RADIUS 5.f
#define DRAG_RADIUS_OFFSET 2.f
#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

#undef max
#undef min

gui_previewwnd::gui_previewwnd(control_pipeline& ctrl_pipeline) :
    ctrl_pipeline(ctrl_pipeline),
    dragging(false), scaling(false), moving(false),
    scale_flags(0),
    sizing_point(0),
    size_point_radius(SIZE_POINT_RADIUS)
{
}

bool gui_previewwnd::select_item(CPoint point, bool& first_selection, bool select_next)
{
    first_selection = false;

    // select item
    if(this->ctrl_pipeline.preview_control->is_disabled())
        return false;

    control_scene* scene = this->ctrl_pipeline.root_scene->get_selected_scene();
    if(!scene || scene->get_video_controls().empty())
        return false;
    auto start_it = scene->get_video_controls().begin();
    if(!this->ctrl_pipeline.get_selected_controls().empty())
    {
        // it is assumed that the selected item is contained in the active scene
        control_video* video_control = dynamic_cast<control_video*>(
            this->ctrl_pipeline.get_selected_controls()[0]);
        if(video_control)
        {
            bool is_video_control, found;
            auto it = scene->find_control_iterator(video_control->name, is_video_control, found);
            if(is_video_control && found)
            {
                if(select_next)
                    start_it = ++it;
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
            *this->ctrl_pipeline.preview_control, point.x, point.y);
        selection_pos = selection_pos * invert_dest_m;

        const D2D1_RECT_F rectangle = video_control->get_rectangle(true);
        if(selection_pos.x >= rectangle.left && selection_pos.y >= rectangle.top &&
            selection_pos.x <= rectangle.right && selection_pos.y <= rectangle.bottom)
        {
            // select the item
            first_selection = this->ctrl_pipeline.get_selected_controls().empty();
            this->ctrl_pipeline.set_selected_control(it->get());
            item_selected = true;
            break;
        }

        it++;
    }
    while(it != start_it);

    // unselect all items
    if(!item_selected)
        this->ctrl_pipeline.set_selected_control(nullptr, control_pipeline::CLEAR);

    return item_selected;
}

void gui_previewwnd::set_timer(UINT timeout_ms)
{
    this->SetTimer(gui_previewwnd::timer_id, timeout_ms);
}

int gui_previewwnd::OnCreate(LPCREATESTRUCT)
{
    // create timer
    this->set_timer(USER_TIMER_MAXIMUM);

    // set the size
    RECT r;
    this->GetClientRect(&r);
    this->width = std::abs(r.right - r.left);
    this->height = std::abs(r.bottom - r.top);

    return 0;
}

void gui_previewwnd::OnDestroy()
{
    this->KillTimer(gui_previewwnd::timer_id);
}

LRESULT gui_previewwnd::OnSize(UINT /*nType*/, CSize /*Extent*/)
{
    if(!this->ctrl_pipeline.preview_control->is_disabled())
    {
        this->update_size();
        /*this->wnd_parent.wnd_maindlg.RedrawWindow();*/
    }
    return 0;
}

void gui_previewwnd::OnRButtonDown(UINT /*nFlags*/, CPoint /*point*/)
{
    if(this->ctrl_pipeline.preview_control->is_disabled())
        return;
    if(this->ctrl_pipeline.get_selected_controls().empty())
        return;
    control_video* video_control = dynamic_cast<control_video*>(
        this->ctrl_pipeline.get_selected_controls()[0]);
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
        control_video* video_control;
        if(!this->ctrl_pipeline.get_selected_controls().empty() &&
            (video_control = 
                dynamic_cast<control_video*>(this->ctrl_pipeline.get_selected_controls()[0])))
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
        if(this->ctrl_pipeline.get_selected_controls().empty())
            return;
        control_video* video_control = dynamic_cast<control_video*>(
            this->ctrl_pipeline.get_selected_controls()[0]);
        if(!video_control)
            return;

        const D2D1_POINT_2F pos = video_control->client_to_canvas(
            *this->ctrl_pipeline.preview_control, point.x, point.y);
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
    if(this->ctrl_pipeline.preview_control->is_disabled())
        return;
    if(this->ctrl_pipeline.get_selected_controls().empty())
        return;
    control_video* video_control = dynamic_cast<control_video*>(
        this->ctrl_pipeline.get_selected_controls()[0]);
    if(!video_control)
        return;

    /*control_video::video_params_t video_params = video_control->get_video_params(true);*/
    D2D1_RECT_F preview_rect = this->get_preview_rect();
    // do not allow dragging if the preview rect has an invalid size
    if(preview_rect.left >= preview_rect.right || preview_rect.top >= preview_rect.bottom)
        return;

    const FLOAT size_point_radius = this->size_point_radius + DRAG_RADIUS_OFFSET;
    D2D1_POINT_2F sizing_points[8];
    video_control->get_sizing_points(this->ctrl_pipeline.preview_control.get(),
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

    if(video_control->is_degenerate())
    {
        // scaling not possible
        this->scale_flags = 0;
        this->scaling = false;
    }

    if(this->dragging)
    {
    dragging:
        D2D1_POINT_2F pos = video_control->client_to_canvas(
            *this->ctrl_pipeline.preview_control, point.x, point.y);
        const D2D1_POINT_2F old_pos = video_control->client_to_canvas(
            *this->ctrl_pipeline.preview_control, this->last_pos.x, this->last_pos.y);
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
                *this->ctrl_pipeline.preview_control, x_clamped, y_clamped, this->sizing_point);
            if(x_clamped || y_clamped)
                video_control->scale(clamping_vector.x, clamping_vector.y, this->scale_flags, false);

            D2D1_POINT_2F points[8];
            video_control->get_sizing_points(nullptr, points, ARRAYSIZE(points));

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
                *this->ctrl_pipeline.preview_control, x_clamped, y_clamped);

            if(x_clamped || y_clamped)
                video_control->move(clamping_vector.x, clamping_vector.y, false, false);
            video_control->apply_transformation();
        }
    }
}

void gui_previewwnd::OnTimer(UINT_PTR uTimerId)
{
    if(uTimerId != gui_previewwnd::timer_id)
        this->SetMsgHandled(FALSE);
    else
        this->RedrawWindow();
}

BOOL gui_previewwnd::OnEraseBkgnd(HDC /*hdc*/)
{
    this->update_preview();
    // background was erased
    return TRUE;
}

void gui_previewwnd::update_preview()
{
    sink_preview2_t preview_window = this->ctrl_pipeline.preview_control->get_component();
    if(!preview_window)
        return;

    UINT32 canvas_width, canvas_height;
    this->ctrl_pipeline.preview_control->get_canvas_size(canvas_width, canvas_height);

    HRESULT hr = S_OK;
    bool has_video_control = false;
    int highlighted_points = 0;
    D2D1_RECT_F dest_rect;
    D2D1::Matrix3x2F dest_m;
    control_video::video_params_t video_params = {0};

    if(this->ctrl_pipeline.get_selected_controls().empty())
        goto out;
    control_video* video_control =
        dynamic_cast<control_video*>(this->ctrl_pipeline.get_selected_controls()[0]);
    if(!video_control)
        goto out;

    has_video_control = true;
    highlighted_points = video_control->get_highlighted_points();
    dest_rect = video_control->get_rectangle(true);
    dest_m = video_control->get_transformation(true);
    video_params = video_control->get_video_params(true);

out:
    using namespace D2D1;
    scoped_lock lock(preview_window->d2d1_context_mutex);

    D2D1_RECT_F preview_rect = this->get_preview_rect();
    bool invert;

    Matrix3x2F canvas_to_preview = Matrix3x2F::Scale((FLOAT)canvas_width, (FLOAT)canvas_height);
    invert = canvas_to_preview.Invert();
    canvas_to_preview = canvas_to_preview *
        Matrix3x2F::Scale(preview_rect.right - preview_rect.left,
            preview_rect.bottom - preview_rect.top);
    canvas_to_preview = canvas_to_preview * Matrix3x2F::Translation(
        preview_rect.left, preview_rect.top);

    CComPtr<ID2D1Bitmap1> bitmap;
    media_buffer_texture_t last_buffer = preview_window->get_last_buffer();
    if(last_buffer)
        bitmap = last_buffer->bitmap;

    preview_window->d2d1devctx->BeginDraw();
    preview_window->d2d1devctx->Clear(ColorF(ColorF::DimGray));

    if(preview_rect.left > std::numeric_limits<FLOAT>::epsilon() &&
        preview_rect.top > std::numeric_limits<FLOAT>::epsilon())
    {
        // draw preview rect
        preview_window->d2d1devctx->SetTransform(canvas_to_preview);
        if(bitmap)
            preview_window->d2d1devctx->DrawBitmap(bitmap, RectF(0.f, 0.f,
            (FLOAT)canvas_width, (FLOAT)canvas_height));

        if(has_video_control)
        {
            /*control_video::video_params_t video_params = video_control->get_video_params(true);*/

            // draw size box
            const FLOAT box_stroke = 1.5f;
            /*Matrix3x2F dest =
                Matrix3x2F::Scale(
                    video_params.rectangle.right - video_params.rectangle.left,
                    video_params.rectangle.bottom - video_params.rectangle.top) *
                Matrix3x2F::Rotation(video_params.rotation) *
                Matrix3x2F::Translation(video_params.rectangle.left, video_params.rectangle.top);*/
            preview_window->d2d1devctx->SetTransform(dest_m * canvas_to_preview);

            /*D2D1_ANTIALIAS_MODE old_mode = this->d2d1devctx->GetAntialiasMode();*/
            /*this->d2d1devctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);*/
            preview_window->d2d1devctx->DrawRectangle(dest_rect,
                preview_window->box_brush, box_stroke, preview_window->stroke_style);
            /*this->d2d1devctx->SetAntialiasMode(old_mode);*/

            // draw corners
            const int highlighted = highlighted_points;
            ID2D1SolidColorBrush* brush;
            FLOAT corner_length = this->size_point_radius;
            FLOAT corner_stroke = 1.5f;
            preview_window->d2d1devctx->SetTransform(Matrix3x2F::Identity());

            const D2D1_POINT_2F
                corner = Point2F(dest_rect.left, dest_rect.top) * dest_m * canvas_to_preview,
                corner2 = Point2F(dest_rect.right, dest_rect.top) * dest_m * canvas_to_preview,
                corner3 = Point2F(dest_rect.left, dest_rect.bottom) * dest_m * canvas_to_preview,
                corner4 = Point2F(dest_rect.right, dest_rect.bottom) * dest_m * canvas_to_preview;

            if((highlighted & control_video::SCALE_LEFT) &&
                (highlighted & control_video::SCALE_TOP))
                brush = preview_window->highlighted_brush, corner_stroke = 3.f;
            else
                brush = preview_window->line_brush, corner_stroke = 1.5f;
            preview_window->d2d1devctx->DrawEllipse(Ellipse(corner, corner_length, corner_length),
                brush, corner_stroke);
            if((highlighted & control_video::SCALE_RIGHT) &&
                (highlighted & control_video::SCALE_TOP))
                brush = preview_window->highlighted_brush, corner_stroke = 3.f;
            else
                brush = preview_window->line_brush, corner_stroke = 1.5f;
            preview_window->d2d1devctx->DrawEllipse(Ellipse(corner2, corner_length, corner_length),
                brush, corner_stroke);
            if((highlighted & control_video::SCALE_LEFT) &&
                (highlighted & control_video::SCALE_BOTTOM))
                brush = preview_window->highlighted_brush, corner_stroke = 3.f;
            else
                brush = preview_window->line_brush, corner_stroke = 1.5f;
            preview_window->d2d1devctx->DrawEllipse(Ellipse(corner3, corner_length, corner_length),
                brush, corner_stroke);
            if((highlighted & control_video::SCALE_RIGHT) &&
                (highlighted & control_video::SCALE_BOTTOM))
                brush = preview_window->highlighted_brush, corner_stroke = 3.f;
            else
                brush = preview_window->line_brush, corner_stroke = 1.5f;
            preview_window->d2d1devctx->DrawEllipse(Ellipse(corner4, corner_length, corner_length),
                brush, corner_stroke);

            // draw lines
            dest_m = Matrix3x2F::Scale(dest_rect.right - dest_rect.left,
                dest_rect.bottom - dest_rect.top) * dest_m;
            const FLOAT dest_rotation = video_params.rotate / 180.f * (FLOAT)M_PI;
            const FLOAT x_scale = cos(dest_rotation), y_scale = sin(dest_rotation);
            corner_stroke = 2.f;
            D2D1_POINT_2F center = Point2F(0.5f, 0.f) * dest_m * canvas_to_preview,
                center2 = Point2F(0.f, 0.5f) * dest_m * canvas_to_preview,
                center3 = Point2F(0.5f, 1.f) * dest_m * canvas_to_preview,
                center4 = Point2F(1.f, 0.5f) * dest_m * canvas_to_preview;

            if(highlighted == control_video::SCALE_TOP)
                brush = preview_window->highlighted_brush, corner_stroke = 3.5f;
            else
                brush = preview_window->line_brush, corner_stroke = 2.f;
            preview_window->d2d1devctx->DrawLine(
                Point2F(center.x - corner_length * y_scale, center.y - corner_length * -x_scale),
                Point2F(center.x + corner_length * y_scale, center.y + corner_length * -x_scale),
                brush, corner_stroke);
            if(highlighted == control_video::SCALE_LEFT)
                brush = preview_window->highlighted_brush, corner_stroke = 3.5f;
            else
                brush = preview_window->line_brush, corner_stroke = 2.f;
            preview_window->d2d1devctx->DrawLine(
                Point2F(center2.x - corner_length * x_scale, center2.y - corner_length * y_scale),
                Point2F(center2.x + corner_length * x_scale, center2.y + corner_length * y_scale),
                brush, corner_stroke);
            if(highlighted == control_video::SCALE_BOTTOM)
                brush = preview_window->highlighted_brush, corner_stroke = 3.5f;
            else
                brush = preview_window->line_brush, corner_stroke = 2.f;
            preview_window->d2d1devctx->DrawLine(
                Point2F(center3.x - corner_length * y_scale, center3.y - corner_length * -x_scale),
                Point2F(center3.x + corner_length * y_scale, center3.y + corner_length * -x_scale),
                brush, corner_stroke);
            if(highlighted == control_video::SCALE_RIGHT)
                brush = preview_window->highlighted_brush, corner_stroke = 3.5f;
            else
                brush = preview_window->line_brush, corner_stroke = 2.f;
            preview_window->d2d1devctx->DrawLine(
                Point2F(center4.x - corner_length * x_scale, center4.y - corner_length * y_scale),
                Point2F(center4.x + corner_length * x_scale, center4.y + corner_length * y_scale),
                brush, corner_stroke);
        }
    }

    CHECK_HR(hr = preview_window->d2d1devctx->EndDraw());

    // dxgi functions need to be synchronized with the context mutex
    {
        scoped_lock lock(*this->ctrl_pipeline.context_mutex);
        CHECK_HR(hr = preview_window->swapchain->Present(0, 0));
    }

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

D2D1_RECT_F gui_previewwnd::get_preview_rect() const
{
    UINT32 canvas_width, canvas_height;
    this->ctrl_pipeline.preview_control->get_canvas_size(canvas_width, canvas_height);

    const FLOAT canvas_w = (FLOAT)canvas_width;
    const FLOAT canvas_h = (FLOAT)canvas_height;
    const FLOAT preview_w = (FLOAT)(this->width - gui_previewwnd::padding_width * 2);
    const FLOAT preview_h = (FLOAT)(this->height - gui_previewwnd::padding_height * 2);

    FLOAT canvas_scale = preview_w / canvas_w;
    FLOAT preview_x = (FLOAT)gui_previewwnd::padding_width,
        preview_y = (FLOAT)gui_previewwnd::padding_height;
    if((canvas_scale * canvas_h) > preview_h)
    {
        canvas_scale = preview_h / canvas_h;
        preview_x = ((FLOAT)this->width - canvas_w * canvas_scale) / 2.f;
    }
    else
        preview_y = ((FLOAT)this->height - canvas_h * canvas_scale) / 2.f;

    return D2D1::RectF(
        preview_x, preview_y,
        preview_x + canvas_w * canvas_scale,
        preview_y + canvas_h * canvas_scale);
}

void gui_previewwnd::update_size()
{
    sink_preview2_t preview_window = this->ctrl_pipeline.preview_control->get_component();
    if(!preview_window)
        return;

    std::lock(*this->ctrl_pipeline.context_mutex, preview_window->d2d1_context_mutex);
    scoped_lock lock(*this->ctrl_pipeline.context_mutex, std::adopt_lock);
    scoped_lock lock2(preview_window->d2d1_context_mutex, std::adopt_lock);

    RECT r;
    this->GetClientRect(&r);
    this->width = std::abs(r.right - r.left);
    this->height = std::abs(r.bottom - r.top);

    HRESULT hr = S_OK;
    CComPtr<IDXGISurface> dxgibackbuffer;
    CComPtr<ID2D1Bitmap1> d2dtarget_bitmap;
    D2D1_BITMAP_PROPERTIES1 bitmap_props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    // reset the device context target
    preview_window->d2d1devctx->SetTarget(nullptr);
    CHECK_HR(hr = preview_window->swapchain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
    // direct2d needs the dxgi version of the backbuffer surface pointer
    CHECK_HR(hr = preview_window->swapchain->GetBuffer(0, IID_PPV_ARGS(&dxgibackbuffer)));
    // get the d2d surface from the dxgi back buffer to use as the d2d render target
    CHECK_HR(hr = preview_window->d2d1devctx->CreateBitmapFromDxgiSurface(
        dxgibackbuffer, &bitmap_props, &d2dtarget_bitmap));

    // now we can set the direct2d render target
    preview_window->d2d1devctx->SetTarget(d2dtarget_bitmap);

done:
    if(FAILED(hr))
        throw HR_EXCEPTION(hr);
}

#pragma warning(pop)