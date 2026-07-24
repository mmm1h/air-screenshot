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
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace airshot::overlay_detail {

class OverlaySession {
public:
    explicit OverlaySession(RegionRequest request, RegionCaptureCompletion completion = {});
    ~OverlaySession();

    bool start();
    RegionResult run();
    void cancel();
    [[nodiscard]] bool active() const noexcept { return started_ && !done_; }
    DragMode hit_test_drag_mode(POINT point) const;
    void on_mouse_down(HWND source, POINT point, bool right);
    void on_mouse_move(POINT point);
    void on_mouse_up(HWND source, POINT point);
    void on_double_click(POINT point);
    void on_key_down(HWND source, WPARAM key);
    void on_mouse_wheel(short delta);
    void show_quick_menu(HWND hwnd, POINT pt);
    void invalidate_all() const;
    [[nodiscard]] RectI display_selection() const;
    [[nodiscard]] int selected_annotation_idx() const noexcept { return selected_annotation_idx_; }
    [[nodiscard]] const RectI& selection() const noexcept { return selection_; }
    [[nodiscard]] bool selection_complete() const noexcept { return selection_complete_; }
    [[nodiscard]] const std::vector<Annotation>& annotations() const noexcept { return annotations_; }
    [[nodiscard]] const Annotation* preview() const noexcept { return drawing_annotation_ ? &preview_ : nullptr; }
    [[nodiscard]] const std::vector<ToolbarButton>& toolbar() const noexcept { return toolbar_; }
    [[nodiscard]] const std::vector<ToolbarButton>& sub_toolbar() const noexcept { return sub_toolbar_; }
    [[nodiscard]] COLORREF active_color() const noexcept { return active_color_; }
    [[nodiscard]] COLORREF custom_color() const noexcept { return custom_color_; }
    [[nodiscard]] float active_width() const noexcept { return active_width_; }
    [[nodiscard]] float active_text_size() const noexcept { return active_text_size_; }
    [[nodiscard]] TextStyle active_text_style() const noexcept { return active_text_style_; }
    [[nodiscard]] int active_highlight_alpha() const noexcept { return active_highlight_alpha_; }
    [[nodiscard]] int mosaic_strength() const noexcept { return mosaic_strength_; }
    [[nodiscard]] int watermark_opacity() const noexcept { return watermark_opacity_; }
    [[nodiscard]] const std::wstring& watermark_text() const noexcept { return watermark_text_; }
    [[nodiscard]] Tool active_tool() const noexcept { return active_tool_; }
    [[nodiscard]] bool annotation_locked_tool() const noexcept { return request_.config.annotation_locked_tool; }
    [[nodiscard]] bool mosaic_is_blur() const noexcept { return mosaic_is_blur_; }
    [[nodiscard]] bool mosaic_is_rect() const noexcept { return mosaic_is_rect_; }
    [[nodiscard]] bool text_size_dropdown_open() const noexcept { return text_size_dropdown_open_; }
    [[nodiscard]] int text_size_hovered_idx() const noexcept { return text_size_hovered_idx_; }
    [[nodiscard]] RectI get_text_size_dropdown_bounds() const noexcept;
    [[nodiscard]] std::wstring hovered_button_id() const noexcept { return hovered_button_id_; }
    [[nodiscard]] bool dragging_selection() const noexcept { return dragging_selection_; }
    [[nodiscard]] POINT cursor_pos() const noexcept { return cursor_pos_; }
    [[nodiscard]] bool color_format_hex() const noexcept { return color_format_hex_; }
    [[nodiscard]] bool is_over_toolbar(POINT point) const noexcept;
    [[nodiscard]] COLORREF get_pixel_color(int x, int y) const noexcept;
    [[nodiscard]] int snap_coordinate(int value, bool is_x, int threshold = 8) const noexcept;
    [[nodiscard]] bool hit_test_annotation(POINT relative) const;

private:
    void build_toolbar();
    void build_sub_toolbar();
    void invoke_sub(std::wstring_view id, HWND source);
    void invoke(std::wstring_view id, HWND source);
    void apply_watermark();
    void finish_annotation();
    bool erase_annotation_at(POINT relative);
    void undo();
    void redo();
    void discard_redo();
    Bitmap original_selection() const;
    Bitmap rendered_selection() const;
    void complete_default(HWND owner);
    void complete_clipboard();
    void complete_file(std::wstring_view requested_path, HWND owner);
    void complete_ocr();
    void complete_pin();
    void complete_scroll(HWND source);
    void run_scroll_capture(HWND source);
    void capture_scroll_frame();
    void finish_scroll_capture(bool cancelled);
    void destroy_scroll_windows();
    void enter_modal() noexcept;
    void leave_modal();
    void deliver_completion();
    void finish(RegionResult result);
    void destroy_windows();

    RegionRequest request_;
    RegionResult result_;
    RegionCaptureCompletion completion_;
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
    bool started_{};
    unsigned int modal_depth_{};
    bool completion_pending_{};
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
    float active_text_size_{18.0F};
    TextStyle active_text_style_{TextStyle::normal};
    int active_highlight_alpha_{96};
    int mosaic_strength_{50};
    int watermark_opacity_{42};
    std::wstring watermark_text_{L"Air Screenshot"};
    std::wstring hovered_button_id_;
    POINT cursor_pos_{};
    bool color_format_hex_{true};
    int selected_annotation_idx_{-1};
    Annotation original_annotation_;
    bool mosaic_is_blur_{};
    bool mosaic_is_rect_{};
    bool text_size_dropdown_open_{};
    int text_size_hovered_idx_{-1};
    HWND prompt_window_{};

    struct ActiveScrollCapture {
        std::unique_ptr<ScrollStitcher> stitcher;
        Bitmap last_frame;
        ScrollControlState control_state;
        HWND border_window{};
        HWND control_window{};
        int locked_direction{};
        int consecutive_failures{};
        bool paused{};
        bool processing{};
        std::wstring pause_text;
    };
    std::unique_ptr<ActiveScrollCapture> scroll_capture_;

    friend class OverlayWindow;
};

}  // namespace airshot::overlay_detail
