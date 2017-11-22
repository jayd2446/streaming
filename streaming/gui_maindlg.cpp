#include "gui_maindlg.h"
#include "gui_newdlg.h"
#include "gui_frame.h"

gui_maindlg::gui_maindlg(gui_frame& wnd_parent) : wnd_parent(wnd_parent)
{
}

void gui_maindlg::add_new_item(new_item_t item)
{
    if(item == NEW_SCENE)
    {
        gui_newdlg dlg(*this, NULL);
        const INT_PTR ret = dlg.DoModal(*this, item);
        if(ret == 0)
        {
            control_scene& scene = this->wnd_parent.ctrl_pipeline.create_scene(dlg.new_scene_name);
            const int index = this->wnd_scene_list.AddString(dlg.new_scene_name.c_str());
            this->wnd_scene_list.SetCurSel(index);

            this->wnd_newvideo.EnableWindow();
            this->wnd_newaudio.EnableWindow();
            scene;
        }
    }
    else if(item == NEW_VIDEO)
    {
        const int index = this->wnd_scene_list.GetCurSel();
        control_scene& scene = this->wnd_parent.ctrl_pipeline.get_scene(index);
        gui_newdlg dlg(*this, &scene);
        const INT_PTR ret = dlg.DoModal(*this, item);
        if(ret == 0)
        {
            scene.add_displaycapture_item(dlg.displaycaptures[dlg.cursel]);
            this->wnd_video_list.AddString(L"Video");

            this->wnd_record.EnableWindow();
        }
    }
    else if(item == NEW_AUDIO)
    {
        const int index = this->wnd_scene_list.GetCurSel();
        control_scene& scene = this->wnd_parent.ctrl_pipeline.get_scene(index);
        gui_newdlg dlg(*this, &scene);
        const INT_PTR ret = dlg.DoModal(*this, item);
        if(ret == 0)
        {
            scene.add_audio_item(dlg.audios[dlg.cursel]);
            this->wnd_audio_list.AddString(dlg.audios[dlg.cursel].device_friendlyname.c_str());

            this->wnd_record.EnableWindow();
        }
    }
}

BOOL gui_maindlg::PreTranslateMessage(MSG* pMsg)
{
    return this->IsDialogMessageW(pMsg);
}

LRESULT gui_maindlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    this->wnd_newscene.Attach(this->GetDlgItem(IDC_NEWSCENE));
    this->wnd_newvideo.Attach(this->GetDlgItem(IDC_NEWVIDEO));
    this->wnd_newaudio.Attach(this->GetDlgItem(IDC_NEWAUDIO));
    this->wnd_record.Attach(this->GetDlgItem(IDC_BUTTON_RECORD));
    this->wnd_scene_list.Attach(this->GetDlgItem(IDC_LIST_SCENES));
    this->wnd_video_list.Attach(this->GetDlgItem(IDC_LIST_VIDEO));
    this->wnd_audio_list.Attach(this->GetDlgItem(IDC_LIST_AUDIO));

    return TRUE;
}

LRESULT gui_maindlg::OnBnClickedNewscene(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    this->add_new_item(NEW_SCENE);
    return 0;
}

LRESULT gui_maindlg::OnBnClickedNewvideo(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    this->add_new_item(NEW_VIDEO);
    return 0;
}

LRESULT gui_maindlg::OnBnClickedNewaudio(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    this->add_new_item(NEW_AUDIO);
    return 0;
}

LRESULT gui_maindlg::OnBnClickedButtonRecord(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    if(!this->wnd_parent.ctrl_pipeline.is_running())
    {
        const int index = this->wnd_scene_list.GetCurSel();
        this->wnd_parent.ctrl_pipeline.set_active(
            this->wnd_parent.ctrl_pipeline.get_scene(index));
        this->wnd_record.SetWindowTextW(L"Stop Recording");
    }
    else
    {
        this->wnd_parent.ctrl_pipeline.set_inactive();
        this->wnd_record.SetWindowTextW(L"Start Recording");
    }

    return 0;
}
