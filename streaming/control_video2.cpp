#include "control_video.h"
#include "control_pipeline2.h"
#include "transform_videomixer.h"
#include "sink_preview2.h"
#define _USE_MATH_DEFINES
#include <math.h>

#undef min
#undef max

// clamp boundary is in client coordinates
#define CLAMP_BOUNDARY 8

control_video::control_video(control_set_t& active_controls, control_pipeline& pipeline) :
    control_class(active_controls, pipeline.mutex), pipeline(pipeline), highlights(0),
    clamp_boundary(CLAMP_BOUNDARY)
{
    this->push_matrix(true);
    this->push_matrix(false);
}

void control_video::push_matrix(bool dest_params)
{
    using namespace D2D1;

    if(dest_params)
        this->transformation_dst.push(this->transformation_dst.empty() ? 
            Matrix3x2F::Identity() : this->transformation_dst.top());
    else
        this->transformation_src.push(this->transformation_src.empty() ?
            Matrix3x2F::Identity() : this->transformation_src.top());
}

void control_video::pop_matrix(bool dest_params)
{
    dest_params ? this->transformation_dst.pop() : this->transformation_src.pop();
}

D2D1::Matrix3x2F control_video::get_parent_transformation(bool dest_params) const
{
    using namespace D2D1;
    const control_video* parent = dynamic_cast<const control_video*>(this->parent);
    if(parent)
        return parent->get_transformation(dest_params);
    
    return Matrix3x2F::Identity();
}

D2D1::Matrix3x2F control_video::get_transformation(bool dest_params) const
{
    using namespace D2D1;
    const Matrix3x2F this_transformation = 
        dest_params ? this->transformation_dst.top() : this->transformation_src.top();

    return (this_transformation * this->get_parent_transformation(dest_params));
}

void control_video::apply_default_video_params()
{
    video_params_t src_params, dst_params;

    this->set_default_video_params(src_params, false);
    this->set_default_video_params(dst_params, true);
    this->build_transformation(src_params, false);
    this->build_transformation(dst_params, true);
    this->apply_transformation(false);
    this->apply_transformation(true);
}

void control_video::build_transformation(const video_params_t& video_params, bool dest_params)
{
    using namespace D2D1;
    Matrix3x2F& transformation = dest_params ? this->transformation_dst.top() : 
        this->transformation_src.top();
    Matrix3x2F parent_transformation_invert = this->get_parent_transformation(dest_params);
    const bool invert = parent_transformation_invert.Invert(); invert;

    transformation = Matrix3x2F::Scale(video_params.scale.x,
        video_params.scale.y) * Matrix3x2F::Rotation(video_params.rotate) *
        Matrix3x2F::Translation(video_params.translate.x, video_params.translate.y) *
        parent_transformation_invert;
}

control_video::video_params_t control_video::get_video_params(D2D1::Matrix3x2F&& m) const
{
    using namespace D2D1;
    video_params_t params;

    params.translate = Point2F(m.dx, m.dy);
    params.scale = Point2F(sqrt(pow(m.m11, 2) + pow(m.m12, 2)), sqrt(pow(m.m21, 2) + pow(m.m22, 2)));
    params.rotate = atan2(m.m12, m.m11) / (FLOAT)M_PI * 180.f;

    // remove the rotation so that scale signs can be extracted
    m = m * Matrix3x2F::Rotation(-params.rotate);
    if(m.m11 < 0.f)
        params.scale.x *= -1.f;
    if(m.m22 < 0.f)
        params.scale.y *= -1.f;

    return params;
}

void control_video::move(FLOAT x, FLOAT y, bool absolute_mode, bool axis_aligned, bool dest_params)
{
    const video_params_t old_params = this->get_video_params(dest_params);
    video_params_t params = old_params;
    using namespace D2D1;

    D2D1_POINT_2F xy = Point2F(x, y);

    if(!axis_aligned)
        xy = xy * Matrix3x2F::Rotation(old_params.rotate);

    if(absolute_mode)
        params.translate = xy;
    else
    {
        params.translate.x += xy.x;
        params.translate.y += xy.y;
    }

    this->build_transformation(params, dest_params);
}

// TODO: rename to stretch
void control_video::scale(FLOAT x, FLOAT y, int scale_type, bool axis_aligned, bool dest_params)
{
    const video_params_t old_params = this->get_video_params(dest_params);
    video_params_t params = old_params;
    using namespace D2D1;

    const D2D1_RECT_F rect = this->get_rectangle(dest_params);
    const D2D1_SIZE_F rect_size = SizeF(rect.right - rect.left, rect.bottom - rect.top);
    D2D1_POINT_2F xy = Point2F(x, y);
    const Matrix3x2F transformation = this->get_transformation(dest_params);
    const int scale_directions = scale_type & (SCALE_LEFT | SCALE_TOP | SCALE_RIGHT | SCALE_BOTTOM);

    if(!axis_aligned)
        xy = xy * Matrix3x2F::Rotation(old_params.rotate);

    if(!(scale_type & ABSOLUTE_MODE))
    {
        const D2D1_POINT_2F old_xy = xy;
        D2D1_POINT_2F points[8];
        this->get_sizing_points(NULL, points, ARRAYSIZE(points), dest_params);

        if((scale_directions & SCALE_LEFT) && (scale_directions & SCALE_TOP))
            xy = points[0];
        if((scale_directions & SCALE_RIGHT) && (scale_directions & SCALE_TOP))
            xy = points[1];
        if((scale_directions & SCALE_LEFT) && (scale_directions & SCALE_BOTTOM))
            xy = points[2];
        if((scale_directions & SCALE_RIGHT) && (scale_directions & SCALE_BOTTOM))
            xy = points[3];
        if(scale_directions == SCALE_TOP)
            xy = points[4];
        if(scale_directions == SCALE_RIGHT)
            xy = points[5];
        if(scale_directions == SCALE_LEFT)
            xy = points[6];
        if(scale_directions == SCALE_BOTTOM)
            xy = points[7];

        xy.x += old_xy.x;
        xy.y += old_xy.y;
    }

    D2D1_POINT_2F position = xy, scale = Point2F(1.f, 1.f), move = Point2F();
    Matrix3x2F transformation_invert = transformation;
    const bool invert = transformation_invert.Invert(); invert;
    position = position * transformation_invert;

    if(scale_type & SCALE_LEFT)
    {
        scale.x = (rect_size.width - position.x) / rect_size.width;
        move.x = position.x;
    }
    if(scale_type & SCALE_TOP)
    {
        scale.y = (rect_size.height - position.y) / rect_size.height;
        move.y = position.y;
    }
    if(scale_type & SCALE_RIGHT)
        scale.x = position.x / rect_size.width;
    if(scale_type & SCALE_BOTTOM)
        scale.y = position.y / rect_size.height;

    if(scale_type & PRESERVE_ASPECT_RATIO)
    {
        if(scale_directions == SCALE_BOTTOM)
        {
            scale.x = scale.y;
            if(scale_type & SCALE_INVERT_CENTER_POINT)
                move.x = rect_size.width - scale.x * rect_size.width;
        }
        else if(scale_directions == SCALE_TOP)
        {
            scale.x = scale.y;
            move.y = rect_size.height - scale.y * rect_size.height;
            if(scale_type & SCALE_INVERT_CENTER_POINT)
                move.x = rect_size.width - scale.x * rect_size.width;
        }
        else if(scale_directions == SCALE_RIGHT)
        {
            scale.y = scale.x;
            if(scale_type & SCALE_INVERT_CENTER_POINT)
                move.y = rect_size.height - scale.y * rect_size.height;
        }
        else if(scale_directions == SCALE_LEFT)
        {
            scale.y = scale.x;
            move.x = rect_size.width - scale.x * rect_size.width;
            if(scale_type & SCALE_INVERT_CENTER_POINT)
                move.y = rect_size.height - scale.y * rect_size.height;
        }
        else
        {
            if((scale.x > scale.y || scale_type & SCALE_USE_X) && !(scale_type & SCALE_USE_Y))
            {
                scale.y = scale.x;
                if(scale_directions & SCALE_TOP)
                    move.y = rect_size.height - scale.y * rect_size.height;
            }
            else
            {
                scale.x = scale.y;
                if(scale_directions & SCALE_LEFT)
                    move.x = rect_size.width - scale.x * rect_size.width;
            }
        }
    }

    Matrix3x2F new_transformation = Matrix3x2F::Scale(scale.x, scale.y) *
        Matrix3x2F::Translation(move.x, move.y);

    new_transformation = new_transformation * transformation;
    params = this->get_video_params(std::move(new_transformation));

    // the image cannot be unscaled if it reaches zero size
    if(std::abs(params.scale.x) < std::numeric_limits<decltype(params.scale.x)>::epsilon())
        params.scale.x = std::numeric_limits<decltype(params.scale.x)>::epsilon();
    if(std::abs(params.scale.y) < std::numeric_limits<decltype(params.scale.y)>::epsilon())
        params.scale.y = std::numeric_limits<decltype(params.scale.y)>::epsilon();

    this->build_transformation(params, dest_params);
}

void control_video::rotate(FLOAT rotation, bool absolute_mode, bool dest_params)
{
    const video_params_t old_params = this->get_video_params(dest_params);
    video_params_t params = old_params;
    using namespace D2D1;

    if(absolute_mode)
    {
        params.rotate = rotation;
    }
    else
    {
        params.rotate += rotation;
    }

    this->build_transformation(params, dest_params);
}

void control_video::align_source_rect()
{
    const video_params_t old_params = this->get_video_params(false),
        old_params_dst = this->get_video_params(true);
    video_params_t params = old_params;
    using namespace D2D1;

    const D2D1_RECT_F src_rect = this->get_rectangle(false),
        dst_rect = this->get_rectangle(true);

    const D2D1_SIZE_F src_size = SizeF(
        src_rect.right - src_rect.left,
        src_rect.bottom - src_rect.top);
    const D2D1_SIZE_F dst_size = SizeF(
        std::abs((dst_rect.right - dst_rect.left) * old_params_dst.scale.x), 
        std::abs((dst_rect.bottom - dst_rect.top) * old_params_dst.scale.y));
    if(dst_size.width < std::numeric_limits<decltype(dst_size.width)>::epsilon() ||
        dst_size.height < std::numeric_limits<decltype(dst_size.height)>::epsilon())
        return;

    FLOAT src_w = src_size.width;
    FLOAT src_h = src_w * dst_size.height / dst_size.width;
    if(src_h < src_size.height)
    {
        src_h = src_size.height;
        src_w = dst_size.width * src_h / dst_size.height;
    }

    const FLOAT scale_w = src_w / src_size.width,
        scale_h = src_h / src_size.height;
    params.scale = Point2F(scale_w, scale_h);
    params.translate = Point2F((src_size.width - src_w) / 2.f, (src_size.height - src_h) / 2.f);

    this->build_transformation(params, false);
}

D2D1_POINT_2F control_video::get_center(bool dest_params) const
{
    using namespace D2D1;
    const D2D1_RECT_F rect = this->get_rectangle(dest_params);
    return (Point2F(rect.left, rect.top) * this->get_transformation(dest_params));
}

D2D1_POINT_2F control_video::get_clamping_vector(const sink_preview2_t& preview_sink, 
    bool& x_clamped, bool& y_clamped, int clamped_sizing_point) const
{
    if(!preview_sink)
        throw HR_EXCEPTION(E_UNEXPECTED);
    if(clamped_sizing_point > 7)
        throw HR_EXCEPTION(E_UNEXPECTED);

    using namespace D2D1;
    const D2D1_RECT_F clamp_edges = RectF(0.f, 0.f, (FLOAT)transform_videomixer::canvas_width,
        (FLOAT)transform_videomixer::canvas_height);
    const D2D1_POINT_2F clamp_boundary = this->client_to_canvas(preview_sink,
        this->clamp_boundary, this->clamp_boundary, true);
    D2D1_POINT_2F clamping_vector = Point2F();
    FLOAT diff, min_diff_x = std::numeric_limits<FLOAT>::max(), 
        min_diff_y = std::numeric_limits<FLOAT>::max();
    x_clamped = y_clamped = false;

    D2D1_POINT_2F corners[8];
    this->get_sizing_points(NULL, corners, ARRAYSIZE(corners));

    if(clamped_sizing_point >= 4)
        clamped_sizing_point -= 4;

    for(int i = 0; i < 4; i++)
    {
        if(clamped_sizing_point >= 0 && i != clamped_sizing_point)
            continue;

        diff = corners[i].x - clamp_edges.left;
        if((std::abs(diff) <= min_diff_x) &&
            std::abs(diff) <= clamp_boundary.x)
        {
            min_diff_x = std::abs(diff);
            clamping_vector.x = -diff;
            x_clamped = true;
        }

        diff = corners[i].y - clamp_edges.top;
        if((std::abs(diff) <= min_diff_y) &&
            std::abs(diff) <= clamp_boundary.y)
        {
            min_diff_y = std::abs(diff);
            clamping_vector.y = -diff;
            y_clamped = true;
        }

        diff = corners[i].x - clamp_edges.right;
        if((std::abs(diff) <= min_diff_x) &&
            std::abs(diff) <= clamp_boundary.x)
        {
            min_diff_x = std::abs(diff);
            clamping_vector.x = -diff;
            x_clamped = true;
        }

        diff = corners[i].y - clamp_edges.bottom;
        if((std::abs(diff) <= min_diff_y) &&
            std::abs(diff) <= clamp_boundary.y)
        {
            min_diff_y = std::abs(diff);
            clamping_vector.y = -diff;
            y_clamped = true;
        }
    }

    return (clamping_vector * Matrix3x2F::Rotation(-this->get_video_params(true).rotate));
}

D2D1_POINT_2F control_video::client_to_canvas(
    const sink_preview2_t& preview_sink, LONG x, LONG y, bool scale_only) const
{
    using namespace D2D1;
    const D2D1_RECT_F&& preview_rect = preview_sink->get_preview_rect();
    const FLOAT canvas_client_ratio_x = (FLOAT)transform_videomixer::canvas_width /
        (preview_rect.right - preview_rect.left);
    const FLOAT canvas_client_ratio_y = (FLOAT)transform_videomixer::canvas_height /
        (preview_rect.bottom - preview_rect.top);
    const Matrix3x2F client_to_canvas = Matrix3x2F::Translation(
        scale_only ? 0 : -preview_rect.left, scale_only ? 0 : -preview_rect.top) * 
        Matrix3x2F::Scale(canvas_client_ratio_x, canvas_client_ratio_y);

    return (Point2F((FLOAT)x, (FLOAT)y) * client_to_canvas);
}

void control_video::get_sizing_points(const sink_preview2_t& preview_sink,
    D2D1_POINT_2F points_out[], int array_size, bool dest_params) const
{
    if(array_size != 8)
        throw HR_EXCEPTION(E_UNEXPECTED);

    using namespace D2D1;
    Matrix3x2F canvas_to_client = Matrix3x2F::Identity();

    if(preview_sink)
    {
        const D2D1_RECT_F&& preview_rect = preview_sink->get_preview_rect();
        canvas_to_client = Matrix3x2F::Scale(
            (preview_rect.right - preview_rect.left) /
            (FLOAT)transform_videomixer::canvas_width,
            (preview_rect.right - preview_rect.left) /
            (FLOAT)transform_videomixer::canvas_width) *
            Matrix3x2F::Translation(preview_rect.left, preview_rect.top);
    }

    const D2D1_RECT_F dest_rectangle = this->get_rectangle(dest_params);
    const Matrix3x2F transformation = this->get_transformation(dest_params) * canvas_to_client;
    const D2D1_SIZE_F rect_size = SizeF(dest_rectangle.right - dest_rectangle.left,
        dest_rectangle.bottom - dest_rectangle.top);

    points_out[0] = Point2F(dest_rectangle.left, dest_rectangle.top) * transformation;
    points_out[1] = Point2F(dest_rectangle.right, dest_rectangle.top) * transformation;
    points_out[2] = Point2F(dest_rectangle.left, dest_rectangle.bottom) * transformation;
    points_out[3] = Point2F(dest_rectangle.right, dest_rectangle.bottom) * transformation;

    points_out[4] = Point2F(dest_rectangle.left + rect_size.width / 2.f, dest_rectangle.top) *
        transformation;
    points_out[5] = Point2F(dest_rectangle.right, dest_rectangle.top + rect_size.height / 2.f) *
        transformation;
    points_out[6] = Point2F(dest_rectangle.left, dest_rectangle.top + rect_size.height / 2.f) *
        transformation;
    points_out[7] = Point2F(dest_rectangle.left + rect_size.width / 2.f, dest_rectangle.bottom) *
        transformation;
}