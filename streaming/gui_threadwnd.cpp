#include "gui_threadwnd.h"
#include "control_class.h"

LRESULT gui_threadwnd::OnGuiThreadMessage(
    UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/)
{
    control_class::callable_f& f = *(control_class::callable_f*)wParam;
    control_class* control = (control_class*)lParam;

    f(control);

    return 0;
}