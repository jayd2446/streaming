#include "gui_dlgs.h"
#include "gui_newdlg.h"

gui_scenedlg::gui_scenedlg(gui_sourcedlg& dlg_sources, const control_pipeline2_t& ctrl_pipeline) :
    ctrl_pipeline(ctrl_pipeline),
    dlg_sources(dlg_sources)
{
}

void gui_scenedlg::add_scene(const std::wstring& scene_name)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

    control_scene2& scene = this->ctrl_pipeline->root_scene.add_scene();
    const int index = this->wnd_scenelist.AddString(scene_name.c_str());
    this->wnd_scenelist.SetCurSel(index);

    // switch to the new scene
    this->ctrl_pipeline->root_scene.switch_scene(scene);

    // set focus to the scene list
    this->wnd_scenelist.SetFocus();

    // set the source tree
    this->dlg_sources.set_source_tree(scene);
}

LRESULT gui_scenedlg::OnBnClickedAddscene(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    gui_newdlg dlg(NULL);
    const INT_PTR ret = dlg.DoModal(*this, gui_newdlg::NEW_SCENE);
    if(ret == 0)
        this->add_scene(dlg.new_scene_name);

    return 0;
}

LRESULT gui_scenedlg::OnBnClickedRemovescene(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    /*control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

    if(this->ctrl_pipeline->is_running())
    {
        for(int i = 0; i < 19; i++)
        {
            this->wnd_scenelist.SetCurSel(i % 2);
            this->ctrl_pipeline->set_active(this->ctrl_pipeline->get_scene(i % 2));
            this->dlg_sources.set_source_tree(*this->ctrl_pipeline->get_active_scene());
        }
    }*/

    return 0;
}

LRESULT gui_scenedlg::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    this->DlgResize_Init(false);

    this->btn_addscene.Attach(this->GetDlgItem(IDC_ADDSCENE));
    this->btn_removescene.Attach(this->GetDlgItem(IDC_REMOVESCENE));
    this->wnd_scenelist.Attach(this->GetDlgItem(IDC_SCENELIST));

    return TRUE;
}

LRESULT gui_scenedlg::OnLbnSelchangeScenelist(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

    if(this->ctrl_pipeline->is_running())
    {
        const int index = this->wnd_scenelist.GetCurSel();
        this->ctrl_pipeline->root_scene.switch_scene(true, index);

        this->dlg_sources.set_source_tree(*this->ctrl_pipeline->root_scene.get_active_scene());
    }

    return 0;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


gui_sourcedlg::gui_sourcedlg(gui_scenedlg& dlg_scenes, const control_pipeline2_t& ctrl_pipeline) :
    ctrl_pipeline(ctrl_pipeline),
    dlg_scenes(dlg_scenes)
{
}

void gui_sourcedlg::set_source_tree(const control_scene2& scene)
{
    this->wnd_sourcetree.DeleteAllItems();

    const control_scene2::controls_t& video_controls = scene.get_video_controls();
    const control_scene2::controls_t& audio_controls = scene.get_audio_controls();

    for(auto&& elem : video_controls)
        this->wnd_sourcetree.InsertItem(elem.first.c_str(), TVI_ROOT, TVI_LAST);
    for(auto&& elem : audio_controls)
        this->wnd_sourcetree.InsertItem(elem.first.c_str(), TVI_ROOT, TVI_LAST);
}

LRESULT gui_sourcedlg::OnBnClickedAddsrc(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

    control_scene2* scene = this->ctrl_pipeline->root_scene.get_active_scene();
    if(!scene)
    {
        this->dlg_scenes.add_scene(L"New Scene");
        scene = this->ctrl_pipeline->root_scene.get_active_scene();
        assert_(scene);
    }

    gui_newdlg dlg(this->ctrl_pipeline);
    const INT_PTR ret = dlg.DoModal(*this, gui_newdlg::NEW_VIDEO);
    if(ret == 0)
    {
        if(dlg.cursel < dlg.audio_sel_offset)
        {
            static int k = 0;
            /*for(int j = 0; j < (k ? 1 : 85); j++)
            {*/
            // video
            static int i = 0;

            const LONG display_w = 
                std::abs(dlg.displaycaptures[dlg.cursel].output.DesktopCoordinates.right - 
                    dlg.displaycaptures[dlg.cursel].output.DesktopCoordinates.left),
                display_h = 
                std::abs(dlg.displaycaptures[dlg.cursel].output.DesktopCoordinates.bottom -
                    dlg.displaycaptures[dlg.cursel].output.DesktopCoordinates.top);

            stream_videoprocessor_controller::params_t params;
            params.source_rect = { 0 };
            params.dest_rect = { 0 };
            params.source_rect.right = display_w - i * 4;
            params.source_rect.bottom = display_h - i * 4;
            params.source_rect.left = i * 4;
            params.source_rect.top = i * 4;

            params.dest_rect.left = i * 4;
            params.dest_rect.top = i * 4;
            params.dest_rect.right =
                std::min((LONG)transform_videoprocessor::canvas_width, display_w) - i * 40;
            params.dest_rect.bottom =
                std::min((LONG)transform_videoprocessor::canvas_height, display_h) - i * 40;
            i++;

            // add the displaycapture and set its params
            control_displaycapture& displaycapture = scene->add_displaycapture();
            displaycapture.videoprocessor_params.reset(new stream_videoprocessor_controller);
            displaycapture.videoprocessor_params->set_params(params);
            displaycapture.set_displaycapture_params(dlg.displaycaptures[dlg.cursel]);

            // TODO: just add items instead of rebuilding the tree
            this->set_source_tree(*scene);

            /*((control_class*)this->ctrl_pipeline.get())->activate();*/

            /*CTreeItem item = this->wnd_sourcetree.InsertItem(L"Video", TVI_ROOT, TVI_LAST);*/
            /*}*/
            k = 1;
        }
        else
        {
            // audio
            static int k = 0;
            /*for(int j = 0; j < 10; j++)
            {*/
            const int index = dlg.cursel - dlg.audio_sel_offset;

            control_wasapi& wasapi = scene->add_wasapi();
            wasapi.set_wasapi_params(dlg.audios[index]);

            // TODO: just add items instead of rebuilding the tree
            this->set_source_tree(*scene);
            /*CTreeItem item = this->wnd_sourcetree.InsertItem(L"Audio", TVI_ROOT, TVI_LAST);*/
            /*((control_class*)this->ctrl_pipeline.get())->activate();*/
            /*}*/
            k = 1;
        }

        ((control_class*)this->ctrl_pipeline.get())->activate();
    }

    return 0;
}

LRESULT gui_sourcedlg::OnBnClickedRemovesrc(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{

    return 0;
}

LRESULT gui_sourcedlg::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    this->DlgResize_Init(false);

    this->btn_addsource.Attach(this->GetDlgItem(IDC_ADDSRC));
    this->btn_removesource.Attach(this->GetDlgItem(IDC_REMOVESRC));
    this->wnd_sourcetree.Attach(this->GetDlgItem(IDC_SOURCETREE));

    return TRUE;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


gui_controldlg::gui_controldlg(const control_pipeline2_t& ctrl_pipeline) : ctrl_pipeline(ctrl_pipeline)
{
}

LRESULT gui_controldlg::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    this->DlgResize_Init(false);

    this->btn_start_recording.Attach(this->GetDlgItem(IDC_START_RECORDING));

    return TRUE;
}

LRESULT gui_controldlg::OnBnClickedStartRecording(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    //bool stop_recording = false;
    //{
    //    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);

    //    if(!this->ctrl_pipeline->get_active_scene())
    //    {
    //        this->MessageBoxW(L"Add some sources first", NULL, MB_ICONINFORMATION);
    //        return 0;
    //    }

    //    if(!this->ctrl_pipeline->is_recording())
    //    {
    //        this->stop_recording_event = this->ctrl_pipeline->start_recording(
    //            L"test.mp4", *this->ctrl_pipeline->get_active_scene());
    //        this->btn_start_recording.SetWindowTextW(L"Stop Recording");
    //    }
    //    else
    //    {
    //        this->ctrl_pipeline->stop_recording();
    //        stop_recording = true;
    //    }
    //}

    //// all locks should be unlocked before calling a blocking function
    //if(stop_recording)
    //{
    //    WaitForSingleObject(this->stop_recording_event, INFINITE);
    //    this->btn_start_recording.SetWindowTextW(L"Start Recording");
    //}

    return 0;
}
