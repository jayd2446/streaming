#pragma once
#include "control_class.h"

// base class for control classes that support video transformations

class sink_preview2;
typedef std::shared_ptr<sink_preview2> sink_preview2_t;

// TODO: control scene will implement the control video, apply_transformation
// will update its own transform first and then update all scene items by calling
// scene_item->apply_transformation();

// TODO: video params t might be unnecessary;
// decomposition of matrix should be used instead(rotation is easily extracted, aswell as translation)
// in case of scaling factor, the matrix should be unrotated first
class control_video2 : public control_class
{
public:
    struct video_params_t
    {
        D2D1_POINT_2F translate, scale;
        FLOAT rotate;
    };
    enum scale_t : int
    {
        SCALE_LEFT = 0x1, SCALE_TOP = 0x2, SCALE_RIGHT = 0x4, SCALE_BOTTOM = 0x8,
        PRESERVE_ASPECT_RATIO = 0x10,
        ABSOLUTE_MODE = 0x20,
    };
private:
    int highlights;
    D2D1::Matrix3x2F transformation_dst, transformation_src;
    
    // builds the transformation by undoing the parent transformation and then applying
    // the parameters
    void build_transformation(const video_params_t& video_params, bool dest_params);
    video_params_t get_video_params(D2D1::Matrix3x2F&&) const;
protected:
    control_pipeline2& pipeline;

    control_video2(control_set_t& active_controls, control_pipeline2&);
    // passed transformation is either dst or src transformation
    virtual void apply_transformation(const D2D1::Matrix3x2F&&, bool dest_params) = 0;
    virtual void set_default_video_params(video_params_t&, bool dest_params) = 0;
public:
    virtual ~control_video2() {}

    virtual D2D1_RECT_F get_rectangle(bool dest_params) const = 0;
    // returns the parent transformation that has the grandparent transformations applied to it
    D2D1::Matrix3x2F get_parent_transformation(bool dest_params) const;
    // returns the transformation that has the parent transformations applied to it
    D2D1::Matrix3x2F get_transformation(bool dest_params) const;
    // decomposes the transformation
    video_params_t get_video_params(bool dest_params) const
    {return this->get_video_params(this->get_transformation(dest_params));}

    // reset transforms functionality in obs
    void apply_default_video_params();
    // used when the parent transformation changes
    void apply_transformation(bool dest_params)
    {this->apply_transformation(this->get_transformation(dest_params), dest_params);}

    void move(const sink_preview2_t&,
        FLOAT x, FLOAT y,
        bool absolute_mode = false,
        bool axis_aligned = true,
        bool dest_params = true);
    void scale(const sink_preview2_t&,
        FLOAT left, FLOAT top, FLOAT right, FLOAT bottom, int scale_type,
        bool axis_aligned = true,
        bool dest_params = true);
    void rotate(FLOAT rotation, bool absolute_mode = false, bool dest_params = true);

    void highlight_sizing_points(int scale_type) { this->highlights = scale_type; }
    int get_highlighted_points() const { return this->highlights; }
    void get_sizing_points(const sink_preview2_t&, D2D1_POINT_2F points_out[], int array_size);
};