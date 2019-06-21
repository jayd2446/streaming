#include "gui_threadwnd.h"

LRESULT gui_threadwnd::OnGuiThreadMessage(
    UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/)
{
    control_class::callable_f& f = *(control_class::callable_f*)wParam;
    control_class_t& control = *(control_class_t*)lParam;

    const bool run = this->ctrl_pipeline->is_active();
    if(run)
        f(control);

    return run ? 1 : 0;
}