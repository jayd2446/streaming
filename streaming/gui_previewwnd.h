#pragma once

#include "wtl.h"
#include "control_pipeline2.h"
#include <d2d1.h>

// preview window implements the user controls for controlling the sources in the
// scene and provides the canvas for sink_preview

class gui_previewwnd : public CWindowImpl<gui_previewwnd>
{
private:
    control_pipeline2_t ctrl_pipeline;
    bool dragging, scaling, moving;
    int scale_flags;
    int sizing_point;
    CPoint last_pos;
    D2D1_POINT_2F pos_to_center;
public:
    DECLARE_WND_CLASS(L"preview")

    explicit gui_previewwnd(const control_pipeline2_t&);

    BEGIN_MSG_MAP(gui_previewwnd)
        MSG_WM_SIZE(OnSize)
        MSG_WM_LBUTTONDOWN(OnLButtonDown)
        MSG_WM_LBUTTONUP(OnLButtonUp)
        MSG_WM_MOUSEMOVE(OnMouseMove)
        MSG_WM_RBUTTONDOWN(OnRButtonDown)
    END_MSG_MAP()

    LRESULT OnSize(UINT nType, CSize Extent);
    void OnRButtonDown(UINT nFlags, CPoint point);
    void OnLButtonDown(UINT nFlags, CPoint point);
    void OnLButtonUp(UINT nFlags, CPoint point);
    void OnMouseMove(UINT nFlags, CPoint point);
};