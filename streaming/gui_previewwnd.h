#pragma once

#include "wtl.h"
#include "control_pipeline.h"

// preview window implements the user controls for controlling the sources in the
// scene and provides the canvas for sink_preview

class gui_previewwnd : public CWindowImpl<gui_previewwnd>
{
private:
    control_pipeline_t ctrl_pipeline;
public:
    DECLARE_WND_CLASS(L"preview")

    explicit gui_previewwnd(const control_pipeline_t&);

    BEGIN_MSG_MAP(gui_previewwnd)
        MSG_WM_SIZE(OnSize)
    END_MSG_MAP()

    LRESULT OnSize(UINT nType, CSize Extent);
};