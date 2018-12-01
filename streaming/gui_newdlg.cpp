#include "gui_newdlg.h"
#include <sstream>

gui_newdlg::gui_newdlg(const control_pipeline2_t& pipeline) : ctrl_pipeline(pipeline)
{
}

LRESULT gui_newdlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
{
    this->new_item = (new_item_t)lParam;
    this->combobox.Attach(this->GetDlgItem(IDC_COMBO_NEW));
    this->editbox.Attach(this->GetDlgItem(IDC_EDIT_NEW));

    switch(new_item)
    {
    case NEW_SCENE:
        this->SetWindowTextW(L"Add New Scene");
        this->editbox.SetWindowTextW(L"New Scene");
        this->combobox.ShowWindow(SW_HIDE);
        break;
    default:
        this->SetWindowTextW(L"Add New Source");
        this->editbox.ShowWindow(SW_HIDE);

        // video
        {
            control_pipeline2::scoped_lock lock(this->ctrl_pipeline->mutex);
            control_displaycapture::list_available_displaycapture_params(this->ctrl_pipeline,
                this->displaycaptures);

            for(auto it = this->displaycaptures.begin(); it != this->displaycaptures.end(); it++)
            {
                const LONG w = 
                    std::abs(it->output.DesktopCoordinates.right - it->output.DesktopCoordinates.left);
                const LONG h = 
                    std::abs(it->output.DesktopCoordinates.bottom - it->output.DesktopCoordinates.top);

                std::wstringstream sts;
                sts << L"Video Source: ";
                sts << it->adapter.Description << L": Monitor ";
                sts << it->output_ordinal << L": " << w << L"x" << h << L" @ ";
                sts << it->output.DesktopCoordinates.left << L"," 
                    << it->output.DesktopCoordinates.bottom;

                this->combobox.AddString(sts.str().c_str());
            }

            this->audio_sel_offset = (int)this->displaycaptures.size();
        }

        // audio
        {
            control_wasapi::list_available_wasapi_params(this->audios);

            for(auto it = this->audios.begin(); it != this->audios.end(); it++)
            {
                std::wstringstream sts;
                sts << L"Audio Source: ";
                if(it->capture)
                    sts << L"Capture Device: ";
                else
                    sts << L"Render Device: ";
                sts << it->device_friendlyname;

                this->combobox.AddString(sts.str().c_str());
            }
        }
        break;
    }

    this->combobox.SetCurSel(0);

    /*this->CenterWindow(this->GetParent());*/

    return TRUE;
}

LRESULT gui_newdlg::OnBnClickedOk(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    if(this->new_item == NEW_SCENE)
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
