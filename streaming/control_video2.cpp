#include "control_video2.h"
#include "control_pipeline2.h"
#include "sink_preview2.h"
#define _USE_MATH_DEFINES
#include <math.h>

control_video2::control_video2(control_set_t& active_controls, control_pipeline2& pipeline) :
    control_class(active_controls, pipeline.mutex), pipeline(pipeline), highlights(0)
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

    return (this->get_parent_transformation(dest_params) * this_transformation);
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
    /*this->apply_transformation(this->get_transformation(false), false);
    this->apply_transformation(this->get_transformation(true), true);*/
}

void control_video2::build_transformation(const video_params_t& video_params, bool dest_params)
{
    using namespace D2D1;
    Matrix3x2F& transformation = dest_params ? this->transformation_dst : this->transformation_src;
    Matrix3x2F parent_transformation_invert = this->get_parent_transformation(dest_params);
    bool invert = parent_transformation_invert.Invert();

    transformation = parent_transformation_invert * Matrix3x2F::Scale(video_params.scale.x,
        video_params.scale.y) * Matrix3x2F::Rotation(video_params.rotate) *
        Matrix3x2F::Translation(video_params.translate.x, video_params.translate.y);
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

void control_video2::move(const sink_preview2_t& preview_sink,
    FLOAT x, FLOAT y, bool absolute_mode, bool axis_aligned, bool dest_params)
{
    video_params_t old_params = this->get_video_params(dest_params);
    video_params_t params = old_params;

    using namespace D2D1;
    D2D1_RECT_F preview_rect = RectF();
    FLOAT canvas_client_ratio = 1.f;

    // TODO: make non axis aligned movement possible

    if(preview_sink)
    {
        preview_rect = preview_sink->get_preview_rect();
        canvas_client_ratio = (FLOAT)transform_videoprocessor2::canvas_width /
            (preview_rect.right - preview_rect.left);
    }
    

    if(absolute_mode)
    {
        const Matrix3x2F client_to_canvas = Matrix3x2F::Translation(-preview_rect.left,
            -preview_rect.top) * Matrix3x2F::Scale(canvas_client_ratio, canvas_client_ratio);

        params.translate = Point2F(x, y) * client_to_canvas;
    }
    else
    {
        const Matrix3x2F client_to_canvas = 
            Matrix3x2F::Scale(canvas_client_ratio, canvas_client_ratio);
        const D2D1_POINT_2F dmove = Point2F(x, y) * client_to_canvas;

        params.translate.x += dmove.x;
        params.translate.y += dmove.y;
    }

    this->build_transformation(params, dest_params);
    this->apply_transformation(dest_params);
}

// TODO: rename to stretch
void control_video2::scale(const sink_preview2_t& preview_sink,
    FLOAT left, FLOAT top, FLOAT right, FLOAT bottom, 
    int scale_type, bool axis_aligned, bool dest_params)
{
    video_params_t old_params = this->get_video_params(dest_params);
    video_params_t params = old_params;

    using namespace D2D1;
    D2D1_RECT_F preview_rect = RectF();
    FLOAT canvas_client_ratio = 1.f;

    // TODO: make non axis aligned movement possible

    if(preview_sink)
    {
        preview_rect = preview_sink->get_preview_rect();
        canvas_client_ratio = (FLOAT)transform_videoprocessor2::canvas_width /
            (preview_rect.right - preview_rect.left);
    }

    if(scale_type & ABSOLUTE_MODE)
    {
        const D2D1_RECT_F rect = this->get_rectangle(dest_params);
        Matrix3x2F client_to_canvas = Matrix3x2F::Translation(-preview_rect.left,
            -preview_rect.top) * Matrix3x2F::Scale(canvas_client_ratio, canvas_client_ratio);
        const D2D1_SIZE_F rect_size = SizeF(rect.right - rect.left, rect.bottom - rect.top);
        const D2D1_POINT_2F left_top = Point2F(left, top) * client_to_canvas,
            right_bottom = Point2F(right, bottom) * client_to_canvas;
        const Matrix3x2F transformation = this->get_transformation(dest_params);

        Matrix3x2F transformation_invert = transformation;
        bool invert = transformation_invert.Invert();
        D2D1_POINT_2F position, scale = Point2F(1.f, 1.f), move = Point2F();

        if(scale_type & SCALE_LEFT)
            position.x = left_top.x;
        if(scale_type & SCALE_TOP)
            position.y = left_top.y;
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
        this->transformation_dst = new_transformation;
    }

    /*this->build_transformation(params, dest_params);*/
    this->apply_transformation(dest_params);
}

void control_video2::rotate(FLOAT rotation, bool absolute_mode, bool dest_params)
{
    video_params_t old_params = this->get_video_params(dest_params);
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
    this->apply_transformation(dest_params);
}

void control_video2::get_sizing_points(
    const sink_preview2_t& preview_sink, D2D1_POINT_2F points_out[], int array_size)
{
    if(array_size != 4)
        throw HR_EXCEPTION(E_UNEXPECTED);

    using namespace D2D1;
    const D2D1_RECT_F preview_rect = preview_sink->get_preview_rect();
    const Matrix3x2F canvas_to_client = Matrix3x2F::Scale(
        (preview_rect.right - preview_rect.left) /
        (FLOAT)transform_videoprocessor2::canvas_width,
        (preview_rect.right - preview_rect.left) /
        (FLOAT)transform_videoprocessor2::canvas_width) *
        Matrix3x2F::Translation(preview_rect.left, preview_rect.top);

    const D2D1_RECT_F dest_rectangle = this->get_rectangle(true);
    const Matrix3x2F transformation = this->get_transformation(true) * canvas_to_client;

    points_out[0] = Point2F(dest_rectangle.left, dest_rectangle.top) * transformation;
    points_out[1] = Point2F(dest_rectangle.right, dest_rectangle.top) * transformation;
    points_out[2] = Point2F(dest_rectangle.left, dest_rectangle.bottom) * transformation;
    points_out[3] = Point2F(dest_rectangle.right, dest_rectangle.bottom) * transformation;
}