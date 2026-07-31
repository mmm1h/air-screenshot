#pragma once

#include "overlay_types.h"

#include "airshot/capture.h"

#include <d2d1.h>
#include <wrl/client.h>

#include <cstdint>

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
    void discard_device_resources() noexcept;
    D2D1_RECT_F local_rect(const RectI& rect) const;
    bool draw_rendered_annotations();
    void draw_annotation(const Annotation& annotation, bool preview);
    void draw_arrow(POINT start,
                    POINT end,
                    ID2D1Brush* brush,
                    float width,
                    ArrowHeadStyle head_style,
                    ID2D1StrokeStyle* stroke_style);

    OverlaySession& session_;
    MonitorSnapshot& monitor_;
    HWND hwnd_{};
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> background_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> rendered_annotations_;
    std::uint64_t rendered_annotation_revision_{};
    RectI rendered_annotation_selection_;
    bool rendered_annotation_has_effect_preview_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> blue_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> white_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> black_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> toolbar_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> toolbar_bg_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> toolbar_border_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover_bg_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> active_bg_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> disabled_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> toolbar_shadow_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> danger_hover_bg_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> true_white_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> green_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> red_brush_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_stroke_style_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashed_stroke_style_;
    bool is_light_theme_{};
};

}  // namespace airshot::overlay_detail
