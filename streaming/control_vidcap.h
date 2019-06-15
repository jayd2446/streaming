#pragma once
#include "control_class.h"
#include "control_video.h"
#include "source_vidcap.h"
#include "transform_videomixer.h"
#include <string>

class control_pipeline;
typedef std::shared_ptr<control_pipeline> control_pipeline2_t;

class control_vidcap : public control_video
{
    friend class control_scene;
public:
    struct vidcap_params
    {
        std::wstring friendly_name;
        std::wstring symbolic_link;
        // TODO: vidcap params also include the parameters that were used to initialize
        // the mf media source object
    };
private:
    control_pipeline& pipeline;
    vidcap_params params;
    source_vidcap_t component;
    stream_videomixer_controller_t videomixer_params;

    media_stream_t stream;
    const control_vidcap* reference;

    // control class
    void build_video_topology(const media_stream_t& from,
        const media_stream_t& to, const media_topology_t&);
    void activate(const control_set_t& last_set, control_set_t& new_set);

    // control video
    void apply_transformation(const D2D1::Matrix3x2F&&, bool dest_params);
    void set_default_video_params(video_params_t&, bool dest_params);

    control_vidcap(control_set_t& active_controls, control_pipeline&);
public:
    // control video
    D2D1_RECT_F get_rectangle(bool dest_params) const;

    // TODO: gui_newdlg should call a control_vidcap function that will 
    // host a format selection dialog for setting up the control_vidcap; 
    // the actual component will be activated on the activate function normally

    // before the vidcap can be activated, right params must be chosen and set
    static void list_available_vidcap_params(const control_pipeline2_t&,
        std::vector<vidcap_params>&);
    void set_vidcap_params(const vidcap_params& params) {this->params = params;}

    bool is_identical_control(const control_class_t&) const;
};