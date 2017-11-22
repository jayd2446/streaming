#pragma once

#include "wtl.h"

class gui_frame;

class gui_preview : public CWindowImpl<gui_preview>
{
private:
    gui_frame& wnd_parent;
public:
    explicit gui_preview(gui_frame&);

    DECLARE_WND_CLASS(L"preview")

    BEGIN_MSG_MAP(gui_preview)
        MSG_WM_SIZE(OnSize)
    END_MSG_MAP()

    LRESULT OnSize(UINT nType, CSize Extent);
};