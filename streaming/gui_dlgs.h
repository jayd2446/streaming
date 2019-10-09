#pragma once

#include "wtl.h"
#include "gui_event_handler.h"
#include "control_pipeline.h"
#include <string>

class gui_scenedlg :
    public gui_event_handler,
    public CDialogImpl<gui_scenedlg>,
    public CDialogResize<gui_scenedlg>
{
private:
    int scene_counter;
    control_pipeline_t ctrl_pipeline;
    CButton btn_addscene, btn_removescene;
    CListBox wnd_scenelist;

    // gui_event_handler
    void on_scene_activate(control_scene* activated_scene, bool deactivated) override;
    void on_control_added(control_class*, bool removed) override;
public:
    enum {IDD = IDD_SCENEDLG};

    explicit gui_scenedlg(const control_pipeline_t&);
    ~gui_scenedlg();

    // command handlers handle events from child windows
    BEGIN_MSG_MAP(gui_scenedlg)
        COMMAND_HANDLER(IDC_ADDSCENE, BN_CLICKED, OnBnClickedAddscene)
        COMMAND_HANDLER(IDC_REMOVESCENE, BN_CLICKED, OnBnClickedRemovescene)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_HANDLER(IDC_SCENELIST, LBN_SELCHANGE, OnLbnSelchangeScenelist)
        CHAIN_MSG_MAP(CDialogResize<gui_scenedlg>)
    END_MSG_MAP()

    BEGIN_DLGRESIZE_MAP(gui_scenedlg)
        DLGRESIZE_CONTROL(IDC_SCENELIST, DLSZ_SIZE_X | DLSZ_SIZE_Y)
        /*BEGIN_DLGRESIZE_GROUP()*/
            DLGRESIZE_CONTROL(IDC_ADDSCENE, DLSZ_MOVE_X | DLSZ_MOVE_Y)
            DLGRESIZE_CONTROL(IDC_REMOVESCENE, DLSZ_MOVE_X | DLSZ_MOVE_Y)
        /*END_DLGRESIZE_GROUP()*/
    END_DLGRESIZE_MAP()

    LRESULT OnBnClickedAddscene(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedRemovescene(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnLbnSelchangeScenelist(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
};

class gui_sourcedlg :
    public gui_event_handler,
    public CDialogImpl<gui_sourcedlg>,
    public CDialogResize<gui_sourcedlg>
{
private:
    int video_counter, audio_counter;

    control_pipeline_t ctrl_pipeline;
    CButton btn_addsource, btn_removesource;
    CTreeViewCtrlEx wnd_sourcetree;
    bool do_not_reselect;
    control_scene* current_active_scene;
    bool update_source_list_on_scene_activate;

    // gui_event_handler
    void on_scene_activate(control_scene* activated_scene, bool deactivated) override;
    void on_activate(control_class*, bool deactivated) override;
    void on_control_added(control_class*, bool removed) override;
    void on_control_selection_changed(bool cleared) override;

    void set_selected_item(CTreeItem item);
    void set_source_tree(const control_scene*);
    void set_selected_item(const control_class*);
public:
    enum {IDD = IDD_SOURCEDLG};

    explicit gui_sourcedlg(const control_pipeline_t&);
    ~gui_sourcedlg();

    BEGIN_MSG_MAP(gui_sourcedlg)
        COMMAND_HANDLER(IDC_ADDSRC, BN_CLICKED, OnBnClickedAddsrc)
        COMMAND_HANDLER(IDC_REMOVESRC, BN_CLICKED, OnBnClickedRemovesrc)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        CHAIN_MSG_MAP(CDialogResize<gui_sourcedlg>)
        NOTIFY_HANDLER(IDC_SOURCETREE, TVN_SELCHANGED, OnTvnSelchangedSourcetree)
        NOTIFY_HANDLER(IDC_SOURCETREE, NM_KILLFOCUS, OnKillFocus)
        NOTIFY_HANDLER(IDC_SOURCETREE, NM_SETFOCUS, OnSetFocus)
        COMMAND_HANDLER(IDC_SRCUP, BN_CLICKED, OnBnClickedSrcup)
        COMMAND_HANDLER(IDC_SRCDOWN, BN_CLICKED, OnBnClickedSrcdown)
    END_MSG_MAP()

    BEGIN_DLGRESIZE_MAP(gui_sourcedlg)
        DLGRESIZE_CONTROL(IDC_SOURCETREE, DLSZ_SIZE_X | DLSZ_SIZE_Y)
        /*BEGIN_DLGRESIZE_GROUP()*/
            DLGRESIZE_CONTROL(IDC_ADDSRC, DLSZ_MOVE_X | DLSZ_MOVE_Y)
            DLGRESIZE_CONTROL(IDC_REMOVESRC, DLSZ_MOVE_X | DLSZ_MOVE_Y)
        /*END_DLGRESIZE_GROUP()*/

        DLGRESIZE_CONTROL(IDC_SRCUP, DLSZ_MOVE_X /*| DLSZ_MOVE_Y*/)
        DLGRESIZE_CONTROL(IDC_SRCDOWN, DLSZ_MOVE_X /*| DLSZ_MOVE_Y*/)
    END_DLGRESIZE_MAP()

    LRESULT OnBnClickedAddsrc(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedRemovesrc(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnTvnSelchangedSourcetree(int /*idCtrl*/, LPNMHDR pNMHDR, BOOL& /*bHandled*/);
    LRESULT OnKillFocus(int /*idCtrl*/, LPNMHDR pNMHDR, BOOL& /*bHandled*/);
    LRESULT OnSetFocus(int /*idCtrl*/, LPNMHDR pNMHDR, BOOL& /*bHandled*/);
    LRESULT OnBnClickedSrcup(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnBnClickedSrcdown(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
};

class gui_controldlg :
    public CDialogImpl<gui_controldlg>,
    public CDialogResize<gui_controldlg>,
    public CIdleHandler
{
private:
    control_pipeline_t ctrl_pipeline;
    CButton btn_start_recording;
public:
    enum {IDD = IDD_CTRLDLG};

    explicit gui_controldlg(const control_pipeline_t&);

    BEGIN_MSG_MAP(gui_controldlg)
        MSG_WM_DESTROY(OnDestroy)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_HANDLER(IDC_START_RECORDING, BN_CLICKED, OnBnClickedStartRecording)
        MESSAGE_HANDLER(RECORDING_STOPPED_MESSAGE, OnRecordingStopped)
        CHAIN_MSG_MAP(CDialogResize<gui_controldlg>)
    END_MSG_MAP()

    BEGIN_DLGRESIZE_MAP(gui_controldlg)
        DLGRESIZE_CONTROL(IDC_START_RECORDING, DLSZ_SIZE_X | DLSZ_MOVE_Y)
    END_DLGRESIZE_MAP()

    void OnDestroy();
    LRESULT OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnBnClickedStartRecording(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
    LRESULT OnRecordingStopped(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);

    BOOL OnIdle();
};