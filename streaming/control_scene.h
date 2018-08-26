#pragma once
#include "control_pipeline.h"
#include "media_topology.h"
#include "source_wasapi.h"
#include "transform_videoprocessor.h"
#include "transform_audioprocessor.h"
#include "transform_audiomixer.h"
#include <mmdeviceapi.h>
#include <string>
#include <vector>
#include <utility>

// control_scene needs to maintain a link between the component and the name of the item;
// components are shared between scenes

class control_scene
{
    friend class control_pipeline;
public:
    // control scene needs to keep the item structs separate from the component
    // structs so that the items can be serialized

    enum video_item_t
    {
        DISPLAYCAPTURE_ITEM
    };

    struct video_item
    {
        RECT source_rect, dest_rect;
        video_item_t type;
    };
    struct displaycapture_item
    {
        video_item video;
        UINT adapter_ordinal, output_ordinal;
        DXGI_ADAPTER_DESC1 adapter;
        DXGI_OUTPUT_DESC output;

        // displaycapture_item based reference index;
        // negative number means no reference;
        // the reference must point to earlier displaycapture_item
        int reference;

        displaycapture_item() : reference(-1) {this->video.type = DISPLAYCAPTURE_ITEM;}
    };
    struct audio_item
    {
        bool capture;
        std::wstring item_name;
        std::wstring device_id;
        std::wstring device_friendlyname;

        int reference;

        audio_item() : reference(-1) {}
    };

    typedef std::vector<video_item> video_items_t;
    typedef std::vector<audio_item> audio_items_t;
private:
    control_pipeline& pipeline;

    // video items is a collection of different types of video items
    video_items_t video_items;
    std::vector<displaycapture_item> displaycapture_items;
    audio_items_t audio_items;

    std::vector<std::pair<displaycapture_item, source_displaycapture5_t>> displaycapture_sources;
    // each video_item will activate a videoprocessor stream controller
    std::vector<stream_videoprocessor_controller_t> videoprocessor_stream_controllers;
    std::vector<std::pair<audio_item, source_audio_t>> audio_sources;

    // finds the non duplicate displaycapture source
    // TODO: obsolete
    /*bool find_displaycapture_source(const displaycapture_item&, source_displaycapture5_t&) const;*/

    bool list_available_audio_items(std::vector<audio_item>& audios, EDataFlow);

    void build_topology(bool reset);
    // called by pipeline
    void activate_scene();
    void deactivate_scene();

    explicit control_scene(control_pipeline&);
public:
    std::wstring scene_name;
    media_topology_t video_topology, audio_topology;

    // returns false if nothing was found
    bool list_available_displaycapture_items(std::vector<displaycapture_item>& displaycaptures);
    bool list_available_audio_items(std::vector<audio_item>& audios);

    // forcing a new instance doesn't create a reference to older displaycapture item
    void add_displaycapture_item(const displaycapture_item&, bool force_new_instance = false);
    /*void remove_video(const std::wstring& item_name);*/
    /*void rename_video(const std::wstring& old_name, const std::wstring& new_name);*/

    void add_audio_item(const audio_item&, bool force_new_instance = false);
    /*void remove_audio(const std::wstring& item_name);*/

    const video_items_t& get_video_items() const {return this->video_items;}
    const audio_items_t& get_audio_items() const {return this->audio_items;}
};