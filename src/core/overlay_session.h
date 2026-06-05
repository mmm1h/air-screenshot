#pragma once

#include "overlay_types.h"
#include "overlay_window.h"

#include "airshot/capture.h"
#include "airshot/ocr.h"
#include "airshot/overlay.h"
#include "airshot/output.h"
#include "airshot/strings.h"
#include "overlay_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace airshot::overlay_detail {

class OverlaySession {
public:
    explicit OverlaySession(RegionRequest request);
    RegionResult run();
    DragMode hit_test_drag_mode(POINT point) const;
    void on_mouse_down(HWND source, POINT point, bool right);
    void on_mouse_move(POINT point);
    void on_mouse_up(POINT point);
    void on_double_click(POINT point);
    void on_key_down(HWND source, WPARAM key);
    void invalidate_all() const;
    [[nodiscard]] RectI display_selection() const;
    [[nodiscard]] const RectI& selection() const noexcept { return selection_; }
    [[nodiscard]] bool selection_complete() const noexcept { return selection_complete_; }
    [[nodiscard]] const std::vector<Annotation>& annotations() const noexcept { return annotations_; }
    [[nodiscard]] const Annotation* preview() const noexcept { return drawing_annotation_ ? &preview_ : nullptr; }
    [[nodiscard]] const std::vector<ToolbarButton>& toolbar() const noexcept { return toolbar_; }
    [[nodiscard]] const std::vector<ToolbarButton>& sub_toolbar() const noexcept { return sub_toolbar_; }
    [[nodiscard]] COLORREF active_color() const noexcept { return active_color_; }
    [[nodiscard]] COLORREF custom_color() const noexcept { return custom_color_; }
    [[nodiscard]] float active_width() const noexcept { return active_width_; }
    [[nodiscard]] Tool active_tool() const noexcept { return active_tool_; }
    [[nodiscard]] std::wstring hovered_button_id() const noexcept { return hovered_button_id_; }
    [[nodiscard]] bool dragging_selection() const noexcept { return dragging_selection_; }
    [[nodiscard]] POINT cursor_pos() const noexcept { return cursor_pos_; }
    [[nodiscard]] bool color_format_hex() const noexcept { return color_format_hex_; }
    [[nodiscard]] bool is_over_toolbar(POINT point) const noexcept;
    [[nodiscard]] COLORREF get_pixel_color(int x, int y) const noexcept;
    [[nodiscard]] int snap_coordinate(int value, bool is_x, int threshold = 8) const noexcept;

private:
    void build_toolbar();
    void build_sub_toolbar();
    void invoke_sub(std::wstring_view id, HWND source);
    void invoke(std::wstring_view id, HWND source);
    void undo();
    void redo();
    void discard_redo();
    Bitmap original_selection() const;
    Bitmap rendered_selection() const;
    void complete_clipboard();
    void complete_file(std::wstring_view requested_path, HWND owner);
    void complete_ocr();
    void complete_pin();
    void complete_scroll(HWND source);
    void run_scroll_capture(HWND source);
    void finish(RegionResult result);

    RegionRequest request_;
    RegionResult result_;
    std::vector<MonitorSnapshot> monitors_;
    std::vector<WindowCandidate> window_candidates_;
    std::vector<std::unique_ptr<OverlayWindow>> windows_;
    RectI virtual_bounds_;
    RectI selection_;
    RectI hover_;
    RectI clicked_window_;
    POINT drag_start_{};
    bool dragging_selection_{};
    bool selection_complete_{};
    bool drawing_annotation_{};
    bool done_{};
    Tool active_tool_{Tool::none};
    Annotation preview_;
    std::vector<Annotation> annotations_;
    std::vector<Annotation> redo_;
    std::vector<ToolbarButton> toolbar_;

    DragMode current_drag_mode_{DragMode::none};
    RectI original_selection_;
    std::vector<ToolbarButton> sub_toolbar_;
    COLORREF active_color_{RGB(245, 34, 45)};
    COLORREF custom_color_{RGB(128, 0, 255)};
    float active_width_{4.0F};
    std::wstring hovered_button_id_;
    POINT cursor_pos_{};
    bool color_format_hex_{true};

    friend class OverlayWindow;
};

}  // namespace airshot::overlay_detail
