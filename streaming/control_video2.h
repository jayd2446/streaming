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
    // in client coords
    LONG clamp_boundary, unclamp_boundary;
    int highlights;
    D2D1::Matrix3x2F transformation_dst, transformation_src;
    
    void set_transformation(const D2D1::Matrix3x2F& m, bool dest_params)
    {dest_params ? this->transformation_dst = m : this->transformation_src = m;}
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
    // used when the parent transformation changes;
    // also used after setting the move/rotate/scale;
    // apply transformation should be called before releasing the pipeline mutex so that
    // the selection rect and the dest rect stays in sync
    void apply_transformation(bool dest_params = true)
    {this->apply_transformation(this->get_transformation(dest_params), dest_params);}

    void move(FLOAT x, FLOAT y,
        bool absolute_mode = false,
        bool axis_aligned = true,
        bool dest_params = true);
    void scale(FLOAT left, FLOAT top, FLOAT right, FLOAT bottom, 
        int scale_type,
        bool axis_aligned = true,
        bool dest_params = true);
    void rotate(FLOAT rotation, bool absolute_mode = false, bool dest_params = true);

    bool allow_clamping(LONG dxy) const {return std::abs(dxy) <= this->unclamp_boundary;}
    // returns the clamping vector in canvas coords;
    // the clamping area is the canvas area;
    // scale type takes flags to exclude points and returns clamping directions
    D2D1_POINT_2F get_clamping_vector(const sink_preview2_t&,
        D2D1_POINT_2F move_vector, int& scale_type) const;

    D2D1_POINT_2F client_to_canvas(const sink_preview2_t&, LONG x, LONG y, 
        bool scale_only = false) const;

    void highlight_sizing_points(int scale_type) { this->highlights = scale_type; }
    int get_highlighted_points() const { return this->highlights; }
    // 0 1 | 4 5
    // 2 3 | 6 7;
    void get_sizing_points(const sink_preview2_t&, D2D1_POINT_2F points_out[], int array_size) const;
};