#include "gui_newdlg.h"
#include "gui_maindlg.h"
#include "gui_frame.h"
#include "control_scene.h"
#include <sstream>

gui_newdlg::gui_newdlg(gui_maindlg& wnd_parent, control_scene* scene) : 
    wnd_parent(wnd_parent), scene(scene)
{
}

LRESULT gui_newdlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
{
    this->new_item = (gui_maindlg::new_item_t)lParam;
    this->combobox.Attach(this->GetDlgItem(IDC_COMBO_NEW));
    this->editbox.Attach(this->GetDlgItem(IDC_EDIT_NEW));

    std::vector<control_scene::audio_item> audio_items;

    switch(new_item)
    {
    case gui_maindlg::NEW_SCENE:
        this->SetWindowTextW(L"Create New Scene");
        this->editbox.SetWindowTextW(L"New Scene");
        this->combobox.ShowWindow(SW_HIDE);
        break;
    case gui_maindlg::NEW_VIDEO:
        this->SetWindowTextW(L"Select Displaycapture Source");
        this->editbox.ShowWindow(SW_HIDE);
        {
            this->scene->list_available_displaycapture_items(this->displaycaptures);

            for(auto it = this->displaycaptures.begin(); it != this->displaycaptures.end(); it++)
            {
                const LONG w = 
                    std::abs(it->output.DesktopCoordinates.right - it->output.DesktopCoordinates.left);
                const LONG h = 
                    std::abs(it->output.DesktopCoordinates.bottom - it->output.DesktopCoordinates.top);

                std::wstringstream sts;
                sts << it->adapter.Description << L": Monitor ";
                sts << it->output_ordinal << L": " << w << L"x" << h << " @ ";
                sts << it->output.DesktopCoordinates.left << "," << it->output.DesktopCoordinates.bottom;

                this->combobox.AddString(sts.str().c_str());
            }
        }
        break;
    case gui_maindlg::NEW_AUDIO:
        this->SetWindowTextW(L"Select Audio Source");
        this->editbox.ShowWindow(SW_HIDE);
        {
            this->scene->list_available_audio_items(this->audios);

            for(auto it = this->audios.begin(); it != this->audios.end(); it++)
            {
                std::wstringstream sts;
                if(it->capture)
                    sts << "Capture Device: ";
                else
                    sts << "Render Device: ";
                sts << it->device_friendlyname;

                this->combobox.AddString(sts.str().c_str());
            }
        }
        break;
    }

    this->combobox.SetCurSel(0);

    return TRUE;
}

LRESULT gui_newdlg::OnBnClickedOk(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    if(this->new_item == gui_maindlg::NEW_SCENE)
    {
        ATL::CString str;
        this->editbox.GetWindowTextW(str);
        if(str.GetLength() == 0)
        {
            this->MessageBoxW(L"Cannot create a scene with empty name.", NULL, MB_ICONERROR);
            return 0;
        }
        this->new_scene_name = str;
    }
    else
        this->cursel = this->combobox.GetCurSel();

    this->EndDialog(0);
    return 0;
}

LRESULT gui_newdlg::OnBnClickedCancel(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    this->EndDialog(1);
    return 0;
}


LRESULT gui_newdlg::OnDestroy(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
{
    return 0;
}
