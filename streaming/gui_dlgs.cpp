#include "gui_dlgs.h"
#include "gui_newdlg.h"
#include <iostream>
#include <sstream>

gui_scenedlg::gui_scenedlg(gui_sourcedlg& dlg_sources, const control_pipeline2_t& ctrl_pipeline) :
    ctrl_pipeline(ctrl_pipeline),
    dlg_sources(dlg_sources),
    scene_counter(0)
{
}

void gui_scenedlg::add_scene(const std::wstring& /*scene_name*/)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

    std::wostringstream sts;
    sts << "scene" << this->scene_counter;
    std::wstring&& str = sts.str();
    control_scene2& scene = *this->ctrl_pipeline->root_scene.add_scene(str);
    this->scene_counter++;

    const int index = this->wnd_scenelist.AddString(str.c_str());
    this->wnd_scenelist.SetCurSel(index);

    // manully trigger the event selection event
    BOOL b;
    this->OnLbnSelchangeScenelist(0, 0, NULL, b);
}

LRESULT gui_scenedlg::OnBnClickedAddscene(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    /*gui_newdlg dlg(NULL);
    const INT_PTR ret = dlg.DoModal(*this, gui_newdlg::NEW_SCENE);
    if(ret == 0)
        this->add_scene(dlg.new_scene_name);*/

    this->add_scene(L"");

    return 0;
}

LRESULT gui_scenedlg::OnBnClickedRemovescene(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

    const int index = this->wnd_scenelist.GetCurSel();
    if(index != LB_ERR)
    {
        const int next_index = (index == 0) ? index + 1 : index - 1;
        const int size = this->wnd_scenelist.GetCount();
        if(size > 1)
        {
            CString name, old_name;
            this->wnd_scenelist.SetCurSel(next_index);
            this->wnd_scenelist.GetText(next_index, name);
            this->wnd_scenelist.GetText(index, old_name);

            bool is_video_control, found;
            bool is_video_control2, found2;
            auto it = this->ctrl_pipeline->root_scene.find_control_iterator(
                std::wstring(name), is_video_control, found),
                old_it = this->ctrl_pipeline->root_scene.find_control_iterator(
                    std::wstring(old_name), is_video_control2, found2);
            control_scene2* scene, *scene2;

            if(found && (scene = dynamic_cast<control_scene2*>(it->get())))
            {
                // switch to new scene
                this->ctrl_pipeline->root_scene.switch_scene(*scene);
                // remove old scene
                if(found2 && is_video_control2 && 
                    (scene2 = dynamic_cast<control_scene2*>(old_it->get())))
                {
                   this->ctrl_pipeline->root_scene.video_controls.erase(old_it);
                }
            }

            this->wnd_scenelist.SetCurSel(next_index);
            this->wnd_scenelist.DeleteString(index);
            this->dlg_sources.set_source_tree(this->ctrl_pipeline->root_scene.get_active_scene());
        }
        else if(size == 1)
        {
            CString old_name;
            this->wnd_scenelist.GetText(index, old_name);

            bool is_video_control2, found2;
            auto old_it = this->ctrl_pipeline->root_scene.find_control_iterator(
                std::wstring(old_name), is_video_control2, found2);
            control_scene2* scene2;

            // deactivate pipeline since it is the last scene and remove old scene
            if(found2 && is_video_control2 &&
                (scene2 = dynamic_cast<control_scene2*>(old_it->get())))
            {
                this->ctrl_pipeline->deactivate();
                this->ctrl_pipeline->root_scene.video_controls.erase(old_it);
                assert_(this->ctrl_pipeline->root_scene.video_controls.empty());
                assert_(this->ctrl_pipeline->root_scene.audio_controls.empty());
            }

            this->wnd_scenelist.SetCurSel(-1);
            this->wnd_scenelist.DeleteString(index);
            this->dlg_sources.set_source_tree(this->ctrl_pipeline->root_scene.get_active_scene());
        }
    }

    return 0;
}

LRESULT gui_scenedlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
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

    const int index = this->wnd_scenelist.GetCurSel();
    this->ctrl_pipeline->root_scene.switch_scene(true, index);

    this->dlg_sources.set_source_tree(this->ctrl_pipeline->root_scene.get_active_scene());

    // set focus to the source dialog
    this->dlg_sources.SetFocus();

    // unselect items
    this->dlg_sources.set_selected_item(NULL);

    return 0;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


gui_sourcedlg::gui_sourcedlg(gui_scenedlg& dlg_scenes, const control_pipeline2_t& ctrl_pipeline) :
    ctrl_pipeline(ctrl_pipeline),
    dlg_scenes(dlg_scenes),
    video_counter(0), audio_counter(0)
{
}

void gui_sourcedlg::set_source_tree(const control_scene2* scene)
{
    this->wnd_sourcetree.DeleteAllItems();

    if(!scene)
        return;

    const control_scene2::controls_t& video_controls = scene->get_video_controls();
    const control_scene2::controls_t& audio_controls = scene->get_audio_controls();

    for(auto&& elem : video_controls)
        this->wnd_sourcetree.InsertItem(elem->name.c_str(), TVI_ROOT, TVI_LAST);
    for(auto&& elem : audio_controls)
        this->wnd_sourcetree.InsertItem(elem->name.c_str(), TVI_ROOT, TVI_LAST);
}

void gui_sourcedlg::set_selected_item(CTreeItem item)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
    if(!item.IsNull())
    {
        CString str;
        item.GetText(str);

        bool is_video_control, found;
        control_scene2* active_scene = this->ctrl_pipeline->root_scene.get_active_scene();
        if(!active_scene)
            return;
        auto it = active_scene->find_control_iterator(str.GetBuffer(), is_video_control, found);

        if(found)
        {
            this->ctrl_pipeline->selected_items.clear();
            this->ctrl_pipeline->selected_items.push_back(it->get());
        }
    }
    else
    {
        this->ctrl_pipeline->selected_items.clear();
    }
}

void gui_sourcedlg::set_selected_item(const control_class* control)
{
    // pipeline lock is assumed when the control is passed as an argument
    CTreeItem first_item = this->wnd_sourcetree.GetNextItem(NULL, TVGN_ROOT);
    CTreeItem item = first_item;
    if(!first_item || !control)
        goto unselect;
    
    do
    {
        CString text;
        item.GetText(text);

        if(control->name.compare(text) == 0)
        {
            // triggers the OnTvnSelchangedSourcetree event handler
            item.Select();
            return;
        }

        item = this->wnd_sourcetree.GetNextSiblingItem(item);
    }
    while(item != first_item);

unselect:
    this->wnd_sourcetree.Select(NULL, TVGN_CARET);
}

LRESULT gui_sourcedlg::OnBnClickedAddsrc(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    control_scene2* scene;
    {
        control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
        scene = this->ctrl_pipeline->root_scene.get_active_scene();
        if(!scene)
        {
            this->dlg_scenes.add_scene(L"New Scene");
            scene = this->ctrl_pipeline->root_scene.get_active_scene();
            assert_(scene);
        }
    }

    gui_newdlg dlg(this->ctrl_pipeline);
    const INT_PTR ret = dlg.DoModal(*this, gui_newdlg::NEW_VIDEO);
    if(ret == 0)
    {
        // set the focus to the source list
        this->wnd_sourcetree.SetFocus();

        control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

        if(dlg.cursel < dlg.audio_sel_offset)
        {
            static int k = 0;
            /*for(int j = 0; j < (k ? 1 : 85); j++)
            {*/
            // video
            static FLOAT i = 0.f;

            const LONG display_w = 
                std::abs(dlg.displaycaptures[dlg.cursel].output.DesktopCoordinates.right - 
                    dlg.displaycaptures[dlg.cursel].output.DesktopCoordinates.left),
                display_h = 
                std::abs(dlg.displaycaptures[dlg.cursel].output.DesktopCoordinates.bottom -
                    dlg.displaycaptures[dlg.cursel].output.DesktopCoordinates.top);

            //stream_videomixer_controller::params_t params;
            //D2D1_RECT_F dest_rect;
            //const FLOAT dest_rot = -i * 100.f;
            //params.source_rect = { 0.f };
            //params.dest_rect = { 0.f };
            //params.source_rect.right = (FLOAT)display_w - i * 40.f;
            //params.source_rect.bottom = (FLOAT)display_h - i * 4.f;
            //params.source_rect.left = i * 4.f;
            //params.source_rect.top = i * 4.f;
            //params.source_m = D2D1::Matrix3x2F::Rotation(-i * 10.f);

            //params.dest_rect = D2D1::RectF(0.f, 0.f, 1.f, 1.f);
            ///*params.dest_angle = -i * 10.f;*/
            //dest_rect.left = i * 4.f;
            //dest_rect.top = i * 4.f;
            //dest_rect.right =
            //    (FLOAT)std::min((LONG)transform_videomixer::canvas_width, display_w) - i * 40.f;
            //dest_rect.bottom =
            //    (FLOAT)std::min((LONG)transform_videomixer::canvas_height, display_h) - i * 40.f;

            //params.dest_m = D2D1::Matrix3x2F::Scale(
            //    dest_rect.right - dest_rect.left, dest_rect.bottom - dest_rect.top) *
            //    D2D1::Matrix3x2F::Rotation(dest_rot) *
            //    D2D1::Matrix3x2F::Translation(dest_rect.left, dest_rect.top);

            //// axis aligned clip is faster if the dest rect is axis aligned
            //params.axis_aligned_clip = (dest_rot == 0.f);

            //// TODO:
            //static short z_order = 0;
            //params.z_order = z_order++;

            i += 1.f;

            // add the displaycapture and set its params
            std::wostringstream sts;
            sts << L"displaycapture" << this->video_counter;
            control_displaycapture& displaycapture = *scene->add_displaycapture(std::move(sts.str()));
            this->video_counter++;

            /*displaycapture.videoprocessor_params.reset(new stream_videoprocessor2_controller);
            displaycapture.videoprocessor_params->set_params(params);*/
            displaycapture.set_displaycapture_params(dlg.displaycaptures[dlg.cursel]);
            // displaycapture params must be set before setting video params
            displaycapture.apply_default_video_params();

            // TODO: just add items instead of rebuilding the tree
            this->set_source_tree(scene);

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

            std::wostringstream sts;
            sts << L"wasapi" << this->audio_counter;
            control_wasapi& wasapi = *scene->add_wasapi(std::move(sts.str()));
            this->audio_counter++;

            wasapi.set_wasapi_params(dlg.audios[index]);

            // TODO: just add items instead of rebuilding the tree
            this->set_source_tree(scene);
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
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

    CTreeItem item = this->wnd_sourcetree.GetSelectedItem();

    if(!item.IsNull())
    {
        CString str;
        item.GetText(str);

        bool is_video_control, found;
        control_scene2* active_scene = this->ctrl_pipeline->root_scene.get_active_scene();
        auto it = active_scene->find_control_iterator(str.GetBuffer(), is_video_control, found);
        control_scene2::controls_t& controls = is_video_control ?
            active_scene->video_controls : active_scene->audio_controls;

        if(found)
        {
            (*it)->deactivate();
            controls.erase(it);

            this->set_source_tree(active_scene);
        }
    }

    /*this->wnd_sourcetree.GetSelectedCount();*/

    return 0;
}

LRESULT gui_sourcedlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    this->DlgResize_Init(false);

    this->btn_addsource.Attach(this->GetDlgItem(IDC_ADDSRC));
    this->btn_removesource.Attach(this->GetDlgItem(IDC_REMOVESRC));
    this->wnd_sourcetree.Attach(this->GetDlgItem(IDC_SOURCETREE));

    return TRUE;
}

LRESULT gui_sourcedlg::OnTvnSelchangedSourcetree(int /*idCtrl*/, LPNMHDR pNMHDR, BOOL& /*bHandled*/)
{
    LPNMTREEVIEW pNMTreeView = reinterpret_cast<LPNMTREEVIEW>(pNMHDR);
    this->set_selected_item(CTreeItem(pNMTreeView->itemNew.hItem, &this->wnd_sourcetree));
    return 0;
}

LRESULT gui_sourcedlg::OnKillFocus(int /*idCtrl*/, LPNMHDR /*pNMHDR*/, BOOL& /*bHandled*/)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
    /*if(this->ctrl_pipeline->get_preview_window())*/
        this->ctrl_pipeline->selected_items.clear();
        /*this->ctrl_pipeline->get_preview_window()->set_size_box(NULL);*/
    return 0;
}

LRESULT gui_sourcedlg::OnSetFocus(int /*idCtrl*/, LPNMHDR /*pNMHDR*/, BOOL& /*bHandled*/)
{
    control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
    control_class* selected_item = NULL;
    if(!this->ctrl_pipeline->selected_items.empty())
        selected_item = this->ctrl_pipeline->selected_items[0];

    this->set_selected_item(selected_item);
    return 0;
}




/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


gui_controldlg::gui_controldlg(const control_pipeline2_t& ctrl_pipeline) :
    ctrl_pipeline(ctrl_pipeline),
    stop_recording(false)
{
}

void gui_controldlg::OnDestroy()
{
    extern CAppModule module_;
    CMessageLoop* msg_loop = module_.GetMessageLoop();
    msg_loop->RemoveIdleHandler(this);
}

LRESULT gui_controldlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    extern CAppModule module_;
    CMessageLoop* msg_loop = module_.GetMessageLoop();
    msg_loop->AddIdleHandler(this);

    this->DlgResize_Init(false);

    this->btn_start_recording.Attach(this->GetDlgItem(IDC_START_RECORDING));

    return TRUE;
}

LRESULT gui_controldlg::OnBnClickedStartRecording(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    if(this->stop_recording)
        return 0;

    {
        control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);

        if(!this->ctrl_pipeline->root_scene.get_active_scene())
        {
            this->MessageBoxW(L"Add some sources first", NULL, MB_ICONINFORMATION);
            return 0;
        }

        if(!this->ctrl_pipeline->is_recording())
        {
            this->stop_recording_event.Close();
            this->stop_recording_event.Attach(this->ctrl_pipeline->start_recording(L"test.mp4"));
            this->btn_start_recording.SetWindowTextW(L"Stop Recording");
        }
        else
        {
            this->ctrl_pipeline->stop_recording();
            this->stop_recording = true;

            /*DWORD ret;
            do
            {
                ret = MsgWaitForMultipleObjectsEx(1, &this->stop_recording_event.m_h, INFINITE,
                    QS_ALLINPUT, 0);
                if(ret == (WAIT_OBJECT_0 + 1))
                {
                    MSG msg;
                    if(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&msg);
                        DispatchMessage(&msg);
                    }
                }
            }
            while(ret != WAIT_OBJECT_0);*/

            /*this->btn_start_recording.EnableWindow(FALSE);
            this->btn_start_recording.SetWindowTextW(L"Stopping...");*/
        }
    }

    //// all locks should be unlocked before calling a blocking function
    if(this->stop_recording)
    {
        WaitForSingleObject(this->stop_recording_event, INFINITE);
        this->btn_start_recording.SetWindowTextW(L"Start Recording");
        this->stop_recording = false;
    }

    return 0;
}

BOOL gui_controldlg::OnIdle()
{
    /*if(!this->stop_recording)
        return FALSE;

    const DWORD ret = MsgWaitForMultipleObjectsEx(1, &this->stop_recording_event.m_h, INFINITE,
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    if(ret == WAIT_OBJECT_0)
    {
        this->stop_recording = false;
        this->btn_start_recording.SetWindowTextW(L"Start Recording");
        this->btn_start_recording.EnableWindow();
    }

    if(ret == WAIT_FAILED)
        throw HR_EXCEPTION(E_UNEXPECTED);*/

    return FALSE;
}
