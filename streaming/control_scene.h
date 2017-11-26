#pragma once
#include "media_topology.h"
#include "source_loopback.h"
#include "source_displaycapture5.h"
#include "transform_videoprocessor.h"
#include "transform_audioprocessor.h"
#include "transform_audiomix.h"
#include <mmdeviceapi.h>
#include <string>
#include <vector>
#include <utility>

// control_scene needs to maintain a link between the component and the name of the item;
// components are shared between scenes

class control_pipeline;

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

        displaycapture_item() {this->video.type = DISPLAYCAPTURE_ITEM;}
    };
    struct audio_item
    {
        bool capture;
        std::wstring item_name;
        std::wstring device_id;
        std::wstring device_friendlyname;
    };
private:
    control_pipeline& pipeline;
    media_topology_t video_topology, audio_topology;

    // video items is a collection of different types of video items
    std::vector<video_item> video_items;
    std::vector<displaycapture_item> displaycapture_items;
    std::vector<audio_item> audio_items;

    std::vector<std::pair<displaycapture_item, source_displaycapture5_t>> displaycapture_sources;
    // each video_item will activate a videoprocessor stream controller
    std::vector<stream_videoprocessor_controller_t> videoprocessor_stream_controllers;
    std::vector<std::pair<audio_item, source_loopback_t>> audio_sources;
    std::vector<transform_audioprocessor_t> audio_processors;
    std::vector<transform_audiomix_t> audio_mixers;

    bool list_available_audio_items(std::vector<audio_item>& audios, EDataFlow);

    void reset_topology(bool create_new);
    // called by pipeline
    void activate_scene();
    void deactivate_scene();

    explicit control_scene(control_pipeline&);
public:
    std::wstring scene_name;

    // returns false if nothing was found
    bool list_available_displaycapture_items(std::vector<displaycapture_item>& displaycaptures);
    bool list_available_audio_items(std::vector<audio_item>& audios);

    void add_displaycapture_item(const displaycapture_item&);
    /*void remove_video(const std::wstring& item_name);*/
    /*void rename_video(const std::wstring& old_name, const std::wstring& new_name);*/

    void add_audio_item(const audio_item&);
    /*void remove_audio(const std::wstring& item_name);*/
};