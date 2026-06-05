#pragma once

#include "overlay_types.h"

#include "airshot/capture.h"

#include <d2d1.h>
#include <wrl/client.h>

namespace airshot::overlay_detail {

class OverlaySession;

void release_overlay_factories();

class OverlayWindow {
public:
    OverlayWindow(OverlaySession& session, MonitorSnapshot& monitor) : session_(session), monitor_(monitor) {}
    ~OverlayWindow() { destroy(); }

    bool create();
    void destroy();
    void invalidate() const;
    void paint();
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] const RectI& bounds() const noexcept { return monitor_.bounds; }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

private:
    bool create_render_target();
    D2D1_RECT_F local_rect(const RectI& rect) const;
    void draw_annotation(const Annotation& annotation, bool preview);
    void draw_arrow(POINT start, POINT end, ID2D1Brush* brush, float width);

    OverlaySession& session_;
    MonitorSnapshot& monitor_;
    HWND hwnd_{};
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> background_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> blue_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> white_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> toolbar_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> toolbar_bg_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> toolbar_border_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover_bg_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> active_bg_brush_;
};

}  // namespace airshot::overlay_detail
