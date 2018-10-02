#pragma once
#include "control_class.h"
#include "source_displaycapture5.h"
#include "transform_videoprocessor.h"
#include <vector>

#define CONTROL_DISPLAYCAPTURE5_TYPE_NAME L"displaycapture"

class control_displaycapture : public control_class
{
    friend class control_scene2;
public:
    static const std::wstring displaycapture_type_name;
    struct displaycapture_params
    {
        UINT adapter_ordinal, output_ordinal;
        DXGI_ADAPTER_DESC1 adapter;
        DXGI_OUTPUT_DESC output;
    };
private:
    control_pipeline2& pipeline;
    displaycapture_params params;
    source_displaycapture5_t component;

    media_stream_t stream, pointer_stream;
    const control_displaycapture* reference;

    void build_video_topology_branch(const media_stream_t& to, const media_topology_t&);
    void activate(const control_set_t& last_set, control_set_t& new_set);

    control_displaycapture(control_set_t& active_controls, control_pipeline2&);
public:
    stream_videoprocessor_controller_t videoprocessor_params;

    // before the displaycapture can be activated, right params must be chosen and set
    static void list_available_displaycapture_params(const control_pipeline2_t&,
        std::vector<displaycapture_params>&);
    // TODO: set displaycapture params sets the initial videoprocessor params aswell
    // set displaycapture params will cause the scene to reactivate itself if it is called
    // while it is active
    void set_displaycapture_params(const displaycapture_params& params) {this->params = params;}

    bool is_identical_control(const control_class*) const;

    /*bool is_activated() const;*/
};