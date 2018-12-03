#include "control_video2.h"
#include "control_pipeline2.h"
#include "sink_preview2.h"
#define _USE_MATH_DEFINES
#include <math.h>

// clamp boundary is in client coordinates
#define CLAMP_BOUNDARY 8
#define UNCLAMP_BOUNDARY 12

extern FLOAT _rot;

control_video2::control_video2(control_set_t& active_controls, control_pipeline2& pipeline) :
    control_class(active_controls, pipeline.mutex), pipeline(pipeline), highlights(0),
    clamp_boundary(CLAMP_BOUNDARY), unclamp_boundary(UNCLAMP_BOUNDARY)
{
}

D2D1::Matrix3x2F control_video2::get_parent_transformation(bool dest_params) const
{
    using namespace D2D1;
    const control_video2* parent = dynamic_cast<const control_video2*>(this->parent);
    if(parent)
        return parent->get_transformation(dest_params);
    
    return Matrix3x2F::Identity();
}

D2D1::Matrix3x2F control_video2::get_transformation(bool dest_params) const
{
    using namespace D2D1;
    const Matrix3x2F this_transformation = 
        dest_params ? this->transformation_dst : this->transformation_src;

    return (this_transformation * this->get_parent_transformation(dest_params));
}

void control_video2::apply_default_video_params()
{
    video_params_t src_params, dst_params;

    this->set_default_video_params(src_params, false);
    this->set_default_video_params(dst_params, true);
    this->build_transformation(src_params, false);
    this->build_transformation(dst_params, true);
    this->apply_transformation(false);
    this->apply_transformation(true);
}

void control_video2::build_transformation(const video_params_t& video_params, bool dest_params)
{
    using namespace D2D1;
    Matrix3x2F& transformation = dest_params ? this->transformation_dst : this->transformation_src;
    Matrix3x2F parent_transformation_invert = this->get_parent_transformation(dest_params);
    const bool invert = parent_transformation_invert.Invert(); invert;

    transformation = Matrix3x2F::Scale(video_params.scale.x,
        video_params.scale.y) * Matrix3x2F::Rotation(video_params.rotate) *
        Matrix3x2F::Translation(video_params.translate.x, video_params.translate.y) *
        parent_transformation_invert;
}

control_video2::video_params_t control_video2::get_video_params(D2D1::Matrix3x2F&& m) const
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

void control_video2::move(FLOAT x, FLOAT y, bool absolute_mode, bool axis_aligned, bool dest_params)
{
    const video_params_t old_params = this->get_video_params(dest_params);
    video_params_t params = old_params;
    using namespace D2D1;

    // TODO: make non axis aligned movement possible

    if(absolute_mode)
        params.translate = Point2F(x, y);
    else
    {
        params.translate.x += x;
        params.translate.y += y;
    }

    this->build_transformation(params, dest_params);
    /*this->apply_transformation(dest_params);*/
}

// TODO: rename to stretch
void control_video2::scale(FLOAT left, FLOAT top, FLOAT right, FLOAT bottom, 
    int scale_type, bool axis_aligned, bool dest_params)
{
    const video_params_t old_params = this->get_video_params(dest_params);
    video_params_t params = old_params;
    using namespace D2D1;

    // TODO: make non axis aligned movement possible

    if(scale_type & ABSOLUTE_MODE)
    {
        const D2D1_RECT_F rect = this->get_rectangle(dest_params);
        const D2D1_SIZE_F rect_size = SizeF(rect.right - rect.left, rect.bottom - rect.top);
        const D2D1_POINT_2F left_top = Point2F(left, top), right_bottom = Point2F(right, bottom);
        const Matrix3x2F transformation = this->get_transformation(dest_params);

        Matrix3x2F transformation_invert = transformation;
        const bool invert = transformation_invert.Invert(); invert;
        D2D1_POINT_2F position = left_top, scale = Point2F(1.f, 1.f), move = Point2F();

        if(scale_type & SCALE_RIGHT)
            position.x = right_bottom.x;
        if(scale_type & SCALE_BOTTOM)
            position.y = right_bottom.y;
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

        Matrix3x2F new_transformation = Matrix3x2F::Scale(scale.x, scale.y) *
            Matrix3x2F::Translation(move.x, move.y);

        new_transformation = new_transformation * transformation;
        params = this->get_video_params(std::move(new_transformation));

        // the image cannot be unscaled if it reaches zero size
        if(std::abs(params.scale.x) <= std::numeric_limits<decltype(params.scale.x)>::epsilon())
            params.scale.x = std::numeric_limits<decltype(params.scale.x)>::epsilon();
        if(std::abs(params.scale.y) <= std::numeric_limits<decltype(params.scale.y)>::epsilon())
            params.scale.y = std::numeric_limits<decltype(params.scale.y)>::epsilon();
    }

    this->build_transformation(params, dest_params);
    /*this->apply_transformation(dest_params);*/
}

void control_video2::rotate(FLOAT rotation, bool absolute_mode, bool dest_params)
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
    /*this->apply_transformation(dest_params);*/
}

D2D1_POINT_2F control_video2::get_clamping_vector(const sink_preview2_t& preview_sink,
    D2D1_POINT_2F move_vector, int& scale_type) const
{
    if(!preview_sink)
        throw HR_EXCEPTION(E_UNEXPECTED);

    using namespace D2D1;
    const D2D1_RECT_F clamp_edges = RectF(0.f, 0.f, (FLOAT)transform_videoprocessor2::canvas_width,
        (FLOAT)transform_videoprocessor2::canvas_height);
    const D2D1_POINT_2F clamp_boundary = this->client_to_canvas(preview_sink,
        this->clamp_boundary, this->clamp_boundary, true);
    D2D1_POINT_2F clamping_vector = Point2F();
    FLOAT diff, max_diff_x = 0.f, max_diff_y = 0.f;
    const int excluded_points = scale_type;
    scale_type = 0;

    D2D1_POINT_2F corners[8];
    this->get_sizing_points(NULL, corners, ARRAYSIZE(corners));

    for(int i = 0; i < 4; i++)
    {
        // clamp the closest points

        switch(i)
        {
        case 0:
            if((excluded_points & SCALE_LEFT) && (excluded_points & SCALE_TOP))
                continue;
            break;
        case 1:
            if((excluded_points & SCALE_RIGHT) && (excluded_points & SCALE_TOP))
                continue;
            break;
        case 2:
            if((excluded_points & SCALE_LEFT) && (excluded_points & SCALE_BOTTOM))
                continue;
            break;
        case 3:
            if((excluded_points & SCALE_RIGHT) && (excluded_points & SCALE_BOTTOM))
                continue;
            break;
        }

        diff = corners[i].x - clamp_edges.left;
        if(std::abs(diff) > max_diff_x && std::abs(diff) <= clamp_boundary.x)
        {
            max_diff_x = std::abs(diff);
            clamping_vector.x = -(diff + move_vector.x);
            scale_type &= ~SCALE_RIGHT;
            scale_type |= SCALE_LEFT;
        }

        diff = corners[i].y - clamp_edges.top;
        if(std::abs(diff) > max_diff_y && std::abs(diff) <= clamp_boundary.y)
        {
            max_diff_y = std::abs(diff);
            clamping_vector.y = -(diff + move_vector.y);
            scale_type &= ~SCALE_BOTTOM;
            scale_type |= SCALE_TOP;
        }

        diff = corners[i].x - clamp_edges.right;
        if(std::abs(diff) > max_diff_x && std::abs(diff) <= clamp_boundary.x)
        {
            max_diff_x = std::abs(diff);
            clamping_vector.x = -(diff + move_vector.x);
            scale_type &= ~SCALE_LEFT;
            scale_type |= SCALE_RIGHT;
        }

        diff = corners[i].y - clamp_edges.bottom;
        if(std::abs(diff) > max_diff_y && std::abs(diff) <= clamp_boundary.y)
        {
            max_diff_y = std::abs(diff);
            clamping_vector.y = -(diff + move_vector.y);
            scale_type &= ~SCALE_TOP;
            scale_type |= SCALE_BOTTOM;
        }
    }

    return clamping_vector;
}

D2D1_POINT_2F control_video2::client_to_canvas(
    const sink_preview2_t& preview_sink, LONG x, LONG y, bool scale_only) const
{
    using namespace D2D1;
    const D2D1_RECT_F&& preview_rect = preview_sink->get_preview_rect();
    const FLOAT canvas_client_ratio_x = (FLOAT)transform_videoprocessor2::canvas_width /
        (preview_rect.right - preview_rect.left);
    const FLOAT canvas_client_ratio_y = (FLOAT)transform_videoprocessor2::canvas_height /
        (preview_rect.bottom - preview_rect.top);
    const Matrix3x2F client_to_canvas = Matrix3x2F::Translation(
        scale_only ? 0 : -preview_rect.left, scale_only ? 0 : -preview_rect.top) * 
        Matrix3x2F::Scale(canvas_client_ratio_x, canvas_client_ratio_y);

    return (Point2F((FLOAT)x, (FLOAT)y) * client_to_canvas);
}

void control_video2::get_sizing_points(const sink_preview2_t& preview_sink,
    D2D1_POINT_2F points_out[], int array_size) const
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
            (FLOAT)transform_videoprocessor2::canvas_width,
            (preview_rect.right - preview_rect.left) /
            (FLOAT)transform_videoprocessor2::canvas_width) *
            Matrix3x2F::Translation(preview_rect.left, preview_rect.top);
    }

    const D2D1_RECT_F dest_rectangle = this->get_rectangle(true);
    const Matrix3x2F transformation = this->get_transformation(true) * canvas_to_client;
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