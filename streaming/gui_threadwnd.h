#pragma once
#include "wtl.h"
#include "control_pipeline.h"
#include <mutex>

// entry point for functions that run in the gui thread context

#define GUI_THREAD_MESSAGE WM_APP

class gui_threadwnd : public CWindowImpl<gui_threadwnd, CWindow, CWinTraits<>>
{
public:
    DECLARE_WND_CLASS(L"gui_threadwnd");

    control_pipeline_t ctrl_pipeline;

    BEGIN_MSG_MAP(gui_threadwnd)
        MESSAGE_HANDLER(GUI_THREAD_MESSAGE, OnGuiThreadMessage)
    END_MSG_MAP()

    LRESULT OnGuiThreadMessage(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
};