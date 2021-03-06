#pragma once
#include "control_class.h"
#include "control_video.h"
#include "source_displaycapture.h"
#include "transform_videomixer.h"
#include <vector>

class control_pipeline;
// TODO: rename typedef
typedef std::shared_ptr<control_pipeline> control_pipeline_t;

class control_displaycapture : public control_video
{
    friend class control_scene;
public:
    struct displaycapture_params
    {
        UINT adapter_ordinal, output_ordinal;
        DXGI_ADAPTER_DESC1 adapter;
        DXGI_OUTPUT_DESC output;
    };
private:
    control_pipeline& pipeline;
    displaycapture_params params;
    source_displaycapture_t component;
    stream_videomixer_controller_t videomixer_params;

    media_stream_t stream, pointer_stream;
    const control_displaycapture* reference;

    // control_class
    void build_video_topology(const media_stream_t& from,
        const media_stream_t& to, const media_topology_t&);
    void activate(const control_set_t& last_set, control_set_t& new_set);

    // control_video
    void apply_transformation(const D2D1::Matrix3x2F&&, bool dest_params);
    void set_default_video_params(video_params_t&, bool dest_params);

    control_displaycapture(control_set_t& active_controls, control_pipeline&);
public:
    // control_video
    D2D1_RECT_F get_rectangle(bool dest_params) const;

    // before the displaycapture can be activated, right params must be chosen and set
    static void list_available_displaycapture_params(const control_pipeline_t&,
        std::vector<displaycapture_params>&);
    // TODO: set displaycapture params sets the initial videoprocessor params aswell
    // set displaycapture params will cause the scene to reactivate itself if it is called
    // while it is active
    void set_displaycapture_params(const displaycapture_params& params) {this->params = params;}

    /*void apply_default_video_params();*/

    // checks if the control's displaycapture parameters match this class' parameters
    bool is_identical_control(const control_class_t&) const;

    /*bool is_activated() const;*/
};