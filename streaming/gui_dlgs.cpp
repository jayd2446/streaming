#include "gui_dlgs.h"
#include "gui_newdlg.h"

gui_scenedlg::gui_scenedlg(gui_sourcedlg& dlg_sources, const control_pipeline_t& ctrl_pipeline) : 
    ctrl_pipeline(ctrl_pipeline),
    dlg_sources(dlg_sources)
{
}

void gui_scenedlg::add_scene(const std::wstring& scene_name)
{
    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);

    control_scene& scene = this->ctrl_pipeline->create_scene(scene_name);
    const int index = this->wnd_scenelist.AddString(scene_name.c_str());
    this->wnd_scenelist.SetCurSel(index);

    // switch to the new scene
    this->ctrl_pipeline->set_active(scene);

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
    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);

    if(this->ctrl_pipeline->is_running())
    {
        for(int i = 0; i < 19; i++)
        {
            this->wnd_scenelist.SetCurSel(i % 2);
            this->ctrl_pipeline->set_active(this->ctrl_pipeline->get_scene(i % 2));
            this->dlg_sources.set_source_tree(*this->ctrl_pipeline->get_active_scene());
        }
    }

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
    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);

    if(this->ctrl_pipeline->is_running())
    {
        const int index = this->wnd_scenelist.GetCurSel();
        this->ctrl_pipeline->set_active(this->ctrl_pipeline->get_scene(index));
        
        this->dlg_sources.set_source_tree(*this->ctrl_pipeline->get_active_scene());
    }

    return 0;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


gui_sourcedlg::gui_sourcedlg(gui_scenedlg& dlg_scenes, const control_pipeline_t& ctrl_pipeline) :
    ctrl_pipeline(ctrl_pipeline),
    dlg_scenes(dlg_scenes)
{
}

void gui_sourcedlg::set_source_tree(const control_scene& scene)
{
    this->wnd_sourcetree.DeleteAllItems();

    const control_scene::video_items_t& video_items = scene.get_video_items();
    const control_scene::audio_items_t& audio_items = scene.get_audio_items();

    // groups are listed first, then video and finally audio

    for(auto&& item : video_items)
        this->wnd_sourcetree.InsertItem(L"Video", TVI_ROOT, TVI_LAST);
    for(auto&& item : audio_items)
        this->wnd_sourcetree.InsertItem(L"Audio", TVI_ROOT, TVI_LAST);
}

LRESULT gui_sourcedlg::OnBnClickedAddsrc(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);

    control_scene* scene = this->ctrl_pipeline->get_active_scene();
    if(!scene)
    {
        this->dlg_scenes.add_scene(L"New Scene");
        scene = this->ctrl_pipeline->get_active_scene();
        assert_(scene);
    }

    gui_newdlg dlg(scene);
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
            dlg.displaycaptures[dlg.cursel].video.source_rect = { 0 };
            dlg.displaycaptures[dlg.cursel].video.dest_rect = { 0 };
            dlg.displaycaptures[dlg.cursel].video.source_rect.right = 1920 - i * 4;
            dlg.displaycaptures[dlg.cursel].video.source_rect.bottom = 1080 - i * 4;
            dlg.displaycaptures[dlg.cursel].video.source_rect.left = i * 4;
            dlg.displaycaptures[dlg.cursel].video.source_rect.top = i * 4;

            dlg.displaycaptures[dlg.cursel].video.dest_rect.left = i * 4;
            dlg.displaycaptures[dlg.cursel].video.dest_rect.top = i * 4;
            dlg.displaycaptures[dlg.cursel].video.dest_rect.right =
                transform_videoprocessor::canvas_width - i * 4;
            dlg.displaycaptures[dlg.cursel].video.dest_rect.bottom =
                transform_videoprocessor::canvas_height - i * 4;
            i++;

            scene->add_displaycapture_item(dlg.displaycaptures[dlg.cursel]);

            // TODO: just add items instead of rebuilding the tree
            this->set_source_tree(*scene);

            this->ctrl_pipeline->set_active(*scene);
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
            scene->add_audio_item(dlg.audios[index]);

            // TODO: just add items instead of rebuilding the tree
            this->set_source_tree(*scene);
            /*CTreeItem item = this->wnd_sourcetree.InsertItem(L"Audio", TVI_ROOT, TVI_LAST);*/
            this->ctrl_pipeline->set_active(*scene);
            /*}*/
            k = 1;
        }

        this->ctrl_pipeline->set_active(*scene);
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


gui_controldlg::gui_controldlg(const control_pipeline_t& ctrl_pipeline) : ctrl_pipeline(ctrl_pipeline)
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
    control_pipeline::scoped_lock lock(this->ctrl_pipeline->mutex);

    if(!this->ctrl_pipeline->get_active_scene())
    {
        this->MessageBoxW(L"Add some sources first", NULL, MB_ICONINFORMATION);
        return 0;
    }

    if(!this->ctrl_pipeline->is_recording())
    {
        this->ctrl_pipeline->start_recording(L"test.mp4", *this->ctrl_pipeline->get_active_scene());
        this->btn_start_recording.SetWindowTextW(L"Stop Recording");
    }
    else
    {
        this->ctrl_pipeline->stop_recording();
        this->btn_start_recording.SetWindowTextW(L"Start Recording");
    }

    return 0;
}
