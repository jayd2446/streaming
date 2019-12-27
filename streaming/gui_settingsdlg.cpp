#include "gui_settingsdlg.h"

extern CAppModule module_;

gui_settingsdlg::gui_settingsdlg(const control_pipeline_t& ctrl_pipeline) :
    ctrl_pipeline(ctrl_pipeline),
    should_update_settings_flag(false)
{
}

void gui_settingsdlg::add_settings_pages(const std::vector<settings_page_t>& pages)
{
    this->settings_pages.insert(this->settings_pages.end(), pages.begin(), pages.end());
}

void gui_settingsdlg::update_settings(control_pipeline_config& config)
{
    for(auto&& item : this->settings_pages)
        item.second->update_settings(config);
}

LRESULT gui_settingsdlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL & /*bHandled*/)
{
    this->wnd_settingstree.Attach(this->GetDlgItem(IDC_SETTINGSTREE));
    this->wnd_static.Attach(this->GetDlgItem(IDC_STATIC1));

    if(this->ctrl_pipeline->is_recording())
    {
        this->wnd_static.SetWindowTextW(L"Cannot change settings while recording");
    }
    else
        this->wnd_static.ShowWindow(SW_HIDE);

    for(auto&& item : this->settings_pages)
    {
        // populate the tree
        const CTreeItem& tree_item = 
            this->wnd_settingstree.InsertItem(item.first.c_str(), TVI_ROOT, TVI_LAST);
        this->settings_pages_map[tree_item.m_hTreeItem] = item;

        // create the dialog
        item.second->create(*this);

        // set the position
        RECT tree_rc;
        this->wnd_settingstree.GetClientRect(&tree_rc);
        this->wnd_settingstree.MapWindowPoints(*this, &tree_rc);

        tree_rc.left += tree_rc.right - tree_rc.left + 7;

        item.second->get_wnd().SetWindowPos(
            HWND_TOP, &tree_rc, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    this->wnd_settingstree.SetFocus();

    return TRUE;
}

LRESULT gui_settingsdlg::OnBnClickedOk(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    try
    {
        for(auto&& item : this->settings_pages)
            if(item.second->should_update_settings())
                this->should_update_settings_flag = true;

        this->EndDialog(IDOK);
    }
    catch(std::logic_error err)
    {
        this->MessageBoxW(L"Invalid settings.", nullptr, MB_ICONERROR);
    }

    return 0;
}


LRESULT gui_settingsdlg::OnBnClickedCancel(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
    this->EndDialog(IDCANCEL);
    return 0;
}


LRESULT gui_settingsdlg::OnSelectionChanged(int /*idCtrl*/, LPNMHDR pNMHDR, BOOL& /*bHandled*/)
{
    LPNMTREEVIEW pNMTreeView = reinterpret_cast<LPNMTREEVIEW>(pNMHDR);
    
    CTreeItem old_item(pNMTreeView->itemOld.hItem, &this->wnd_settingstree);
    CTreeItem item(pNMTreeView->itemNew.hItem, &this->wnd_settingstree);
    auto it = this->settings_pages_map.find(item);
    if(it != this->settings_pages_map.end())
    {
        auto old_it = this->settings_pages_map.find(old_item);
        if(old_it != this->settings_pages_map.end())
            old_it->second.second->get_wnd().ShowWindow(SW_HIDE);

        it->second.second->get_wnd().ShowWindow(SW_SHOW);
    }

    return 0;
}


LRESULT gui_settingsdlg::OnCtlColorStatic(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    CStatic static_control = (HWND)lParam;
    if(static_control.m_hWnd == this->wnd_static.m_hWnd)
    {
        // paint the static text red
        CDCHandle dc = (HDC)wParam;
        dc.SetBkMode(TRANSPARENT);
        dc.SetTextColor(RGB(255, 0, 0));

        return (LRESULT)GetStockObject(NULL_BRUSH);
    }

    bHandled = FALSE;
    return 0;
}
