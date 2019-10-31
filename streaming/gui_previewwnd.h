#pragma once

#include "wtl.h"
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <memory>
#include <mutex>

class gui_sourcedlg;
class control_pipeline;

// preview window implements the user controls for controlling the sources in the
// scene and provides the canvas for sink_preview

class gui_previewwnd : public CWindowImpl<gui_previewwnd>
{
public:
    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    static const UINT32 padding_width = 20;
    static const UINT32 padding_height = 20;
private:
    static constexpr UINT_PTR timer_id = 1;

    control_pipeline& ctrl_pipeline;
    bool dragging, scaling, moving;
    int scale_flags;
    int sizing_point;
    CPoint last_pos;
    D2D1_POINT_2F pos_to_center;

    // the radius is in preview window coordinates
    FLOAT size_point_radius;
    UINT width, height;
    std::recursive_mutex d2d1_context_mutex;
    CComPtr<ID2D1Factory1> d2d1factory;
    CComPtr<ID2D1HwndRenderTarget> rendertarget;
    CComPtr<ID2D1Device> d2d1dev;
    CComPtr<ID2D1DeviceContext> d2d1devctx;
    CComPtr<ID3D11Device> d3d11dev;
    CComPtr<IDXGISwapChain1> swapchain;
    CComPtr<IDXGIDevice1> dxgidev;
    CComPtr<ID3D11DeviceContext> d3d11devctx;
    CComPtr<ID3D11RenderTargetView> render_target_view;
    CComPtr<ID2D1SolidColorBrush> box_brush;
    CComPtr<ID2D1SolidColorBrush> line_brush;
    CComPtr<ID2D1SolidColorBrush> highlighted_brush;
    CComPtr<ID2D1StrokeStyle1> stroke_style;

    bool select_item(CPoint point, bool& first_selection, bool select_next = false);
    void update_size();
public:
    DECLARE_WND_CLASS(L"gui_previewwnd")

    explicit gui_previewwnd(control_pipeline&);

    void set_timer(UINT timeout_ms);
    void update_preview();

    // in preview window coordinates
    D2D1_RECT_F get_preview_rect() const;

    BEGIN_MSG_MAP(gui_previewwnd)
        MSG_WM_CREATE(OnCreate)
        MSG_WM_DESTROY(OnDestroy)
        MSG_WM_SIZE(OnSize)
        MSG_WM_LBUTTONDOWN(OnLButtonDown)
        MSG_WM_LBUTTONUP(OnLButtonUp)
        MSG_WM_MOUSEMOVE(OnMouseMove)
        MSG_WM_RBUTTONDOWN(OnRButtonDown)
        MSG_WM_TIMER(OnTimer)
        MSG_WM_ERASEBKGND(OnEraseBkgnd)
    END_MSG_MAP()

    int OnCreate(LPCREATESTRUCT);
    void OnDestroy();
    LRESULT OnSize(UINT nType, CSize Extent);
    void OnRButtonDown(UINT nFlags, CPoint point);
    void OnLButtonDown(UINT nFlags, CPoint point);
    void OnLButtonUp(UINT nFlags, CPoint point);
    void OnMouseMove(UINT nFlags, CPoint point);
    void OnTimer(UINT_PTR uTimerId);
    BOOL OnEraseBkgnd(HDC hdc);
};