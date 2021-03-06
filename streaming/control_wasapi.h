#pragma once
#include "control_class.h"
#include "source_wasapi.h"
#include "transform_audiomixer2.h"
#include <vector>

class control_wasapi : public control_class
{
    friend class control_scene;
public:
    struct wasapi_params
    {
        bool capture;
        std::wstring device_id;
        std::wstring device_friendlyname;
    };
private:
    control_pipeline& pipeline;
    wasapi_params params;
    source_wasapi_t component;
    stream_audiomixer2_controller_t audiomixer_params;

    // stream and reference enable multiple controls to use the same
    // component;
    // for this to work, the topology building must have the same order
    // as the activation, which is guaranteed by the assumption that the
    // topology branches are built in the control_set_t order
    media_stream_t stream;
    const control_wasapi* reference;

    void build_audio_topology(const media_stream_t& from,
        const media_stream_t& to, const media_topology_t&);
    void activate(const control_set_t& last_set, control_set_t& new_set);

    control_wasapi(control_set_t& active_controls, control_pipeline&);
public:
    // before the wasapi can be activated, right params must be chosen and set
    static void list_available_wasapi_params(std::vector<wasapi_params>&);
    // set wasapi params will cause the scene to reactivate itself if it is called
    // while it is active
    void set_wasapi_params(const wasapi_params& params) {this->params = params;}

    bool is_identical_control(const control_class_t&) const;

    /*bool is_activated() const;*/
};