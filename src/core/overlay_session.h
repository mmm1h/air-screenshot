#pragma once

#include "overlay_annotation_history.h"
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
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace airshot::overlay_detail {

inline constexpr UINT kOverlayOcrCompletedMessage = WM_APP + 0x131;
inline constexpr UINT kOverlayScrollFrameCompletedMessage = WM_APP + 0x132;

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
    void on_capture_lost();
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
    [[nodiscard]] ShapeFillStyle active_fill_style() const noexcept {
        return active_fill_style_;
    }
    [[nodiscard]] StrokePattern active_stroke_pattern() const noexcept {
        return active_stroke_pattern_;
    }
    [[nodiscard]] ArrowHeadStyle active_arrow_head_style() const noexcept {
        return active_arrow_head_style_;
    }
    [[nodiscard]] bool active_rectangle_rounded() const noexcept {
        return active_rectangle_rounded_;
    }
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
    [[nodiscard]] RectI dimension_badge_bounds() const noexcept;
    [[nodiscard]] bool dimension_badge_hovered() const noexcept {
        return dimension_badge_hovered_;
    }
    [[nodiscard]] bool can_edit_selection_size() const noexcept;
    [[nodiscard]] bool color_format_hex() const noexcept { return color_format_hex_; }
    [[nodiscard]] bool is_over_toolbar(POINT point) const noexcept;
    [[nodiscard]] bool is_over_toolbar_drag_handle(POINT point) const noexcept;
    [[nodiscard]] bool toolbar_dragging() const noexcept { return dragging_toolbar_; }
    [[nodiscard]] std::uint64_t annotation_revision() const noexcept {
        return annotation_revision_;
    }
    [[nodiscard]] bool annotation_transaction_active() const noexcept {
        return annotation_transaction_before_.has_value();
    }
    [[nodiscard]] COLORREF get_pixel_color(int x, int y) const noexcept;
    [[nodiscard]] int snap_coordinate(int value, bool is_x, int threshold = 8) const noexcept;
    [[nodiscard]] bool hit_test_annotation(POINT relative) const;
    [[nodiscard]] bool ocr_running() const noexcept { return ocr_running_; }
    [[nodiscard]] std::wstring_view ocr_status_text() const noexcept {
        return ocr_cancelling_ ? L"正在取消文字识别…" : L"正在识别文字…  按 Esc 取消";
    }

private:
    [[nodiscard]] unsigned int ui_dpi_at(POINT point) const noexcept;
    void build_toolbar();
    void build_sub_toolbar();
    void invoke_sub(std::wstring_view id, HWND source);
    void invoke(std::wstring_view id, HWND source);
    void apply_watermark();
    void finish_annotation();
    bool erase_annotation_at(POINT relative);
    void record_annotation_change();
    void mark_annotation_visual_changed() noexcept;
    void mark_preview_visual_changed() noexcept;
    void begin_annotation_transaction();
    void mark_annotation_transaction_changed() noexcept;
    void commit_annotation_transaction();
    void cancel_annotation_transaction();
    [[nodiscard]] AnnotationHandle selected_annotation_handle_at(
        POINT point) const noexcept;
    void reset_annotation_drag_state() noexcept;
    void duplicate_selected_annotation();
    void open_selection_size_prompt(HWND source);
    [[nodiscard]] Tool style_context_tool() const noexcept;
    void load_persisted_tool_styles() noexcept;
    void remember_active_style(Tool tool) noexcept;
    void remember_current_style() noexcept;
    void load_active_style(Tool tool) noexcept;
    void sync_active_style_from_selected();
    void apply_active_style_to_selected();
    void edit_selected_text(HWND source);
    [[nodiscard]] bool settle_active_interaction(
        HWND source,
        InteractionSettleMode mode);
    [[nodiscard]] bool cancel_active_interaction(HWND source);
    void release_capture_if_owned(HWND source) noexcept;
    void undo();
    void redo();
    Bitmap original_selection() const;
    [[nodiscard]] const Bitmap& cached_rendered_selection() const;
    [[nodiscard]] bool effect_preview_active() const noexcept;
    [[nodiscard]] std::uint64_t rendered_source_revision() const noexcept;
    [[nodiscard]] const Bitmap& cached_rendered_selection_for_display() const;
    Bitmap rendered_selection() const;
    void complete_default(HWND owner);
    void complete_clipboard();
    void complete_file(std::wstring_view requested_path, HWND owner);
    void complete_ocr();
    void cancel_ocr();
    void handle_ocr_completion();
    void stop_ocr_worker();
    void complete_pin();
    void complete_scroll(HWND source);
    void run_scroll_capture(HWND source);
    void capture_scroll_frame();
    void handle_scroll_frame_completion();
    void toggle_scroll_pause();
    void stop_scroll_worker();
    void finish_scroll_capture(bool cancelled);
    void destroy_scroll_windows();
    [[nodiscard]] HWND modal_owner(HWND preferred = nullptr) const noexcept;
    void show_output_error(HWND owner, std::wstring_view message);
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
    std::wstring capture_topology_signature_;
    RectI selection_;
    RectI hover_;
    RectI clicked_window_;
    std::size_t hover_ancestor_offset_{};
    std::optional<POINT> hover_cycle_anchor_;
    POINT drag_start_{};
    POINT annotation_anchor_{};
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
    AnnotationHistory annotation_history_;
    std::optional<std::vector<Annotation>> annotation_transaction_before_;
    bool annotation_transaction_changed_{};
    std::uint64_t annotation_revision_{1};
    std::uint64_t visual_revision_{1};
    mutable Bitmap rendered_selection_cache_;
    mutable std::uint64_t rendered_selection_cache_revision_{};
    mutable RectI rendered_selection_cache_bounds_;
    mutable Bitmap effect_preview_cache_;
    mutable std::uint64_t effect_preview_cache_revision_{};
    mutable RectI effect_preview_cache_bounds_;
    std::vector<ToolbarButton> toolbar_;
    bool dragging_toolbar_{};
    bool dragging_slider_{};
    std::wstring dragging_slider_id_;
    int dragging_slider_start_value_{};
    bool releasing_capture_{};
    POINT toolbar_drag_start_{};
    POINT toolbar_drag_origin_{};
    std::optional<POINT> toolbar_custom_origin_;

    DragMode current_drag_mode_{DragMode::none};
    RectI original_selection_;
    std::vector<ToolbarButton> sub_toolbar_;
    COLORREF active_color_{RGB(245, 34, 45)};
    COLORREF custom_color_{RGB(128, 0, 255)};
    float active_width_{4.0F};
    float active_text_size_{18.0F};
    TextStyle active_text_style_{TextStyle::normal};
    ShapeFillStyle active_fill_style_{ShapeFillStyle::outline};
    StrokePattern active_stroke_pattern_{StrokePattern::solid};
    ArrowHeadStyle active_arrow_head_style_{ArrowHeadStyle::forward};
    bool active_rectangle_rounded_{};
    int active_highlight_alpha_{96};
    int mosaic_strength_{50};
    int watermark_opacity_{42};
    std::wstring watermark_text_{L"Air Screenshot"};
    std::wstring hovered_button_id_;
    bool dimension_badge_hovered_{};
    POINT cursor_pos_{};
    bool color_format_hex_{true};
    int selected_annotation_idx_{-1};
    Annotation original_annotation_;
    AnnotationHandle active_annotation_handle_{AnnotationHandle::none};
    int annotation_drag_source_idx_{-1};
    bool clone_annotation_on_drag_{};
    bool annotation_drag_clone_created_{};
    bool mosaic_is_blur_{};
    bool mosaic_is_rect_{};
    bool highlight_constraint_active_{};
    ToolStylePalette tool_styles_;
    bool text_size_dropdown_open_{};
    int text_size_hovered_idx_{-1};
    HWND prompt_window_{};

    enum class ScrollFrameStatus {
        cancelled,
        capture_failed,
        target_changed,
        unchanged,
        mismatch,
        reverse_direction,
        stitched,
        stitch_limit,
        stitch_allocation_failed,
        stitch_failed,
    };

    struct ScrollFrameCompletion {
        ScrollFrameStatus status{ScrollFrameStatus::capture_failed};
        int direction{};
        int stitched_height{};
    };

    struct ActiveScrollCapture {
        std::unique_ptr<ScrollStitcher> stitcher;
        Bitmap last_frame;
        ScrollControlState control_state;
        HWND border_window{};
        HWND control_window{};
        HWND keyboard_sink{};
        HHOOK keyboard_hook{};
        int locked_direction{};
        int consecutive_failures{};
        bool paused{};
        bool processing{};
        std::wstring pause_text;
        HWND target_window{};
        DWORD target_process_id{};
        std::mutex frame_mutex;
        std::optional<ScrollFrameCompletion> frame_completion;
        std::jthread frame_worker;
    };
    std::unique_ptr<ActiveScrollCapture> scroll_capture_;

    struct OcrCompletion {
        OcrOutput output;
        bool cancelled{};
    };
    std::mutex ocr_mutex_;
    std::optional<OcrCompletion> ocr_completion_;
    std::jthread ocr_thread_;
    std::wstring pending_ocr_text_;
    bool ocr_running_{};
    bool ocr_cancelling_{};

    friend class OverlayWindow;
};

}  // namespace airshot::overlay_detail
