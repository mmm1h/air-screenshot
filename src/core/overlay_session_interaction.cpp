#include "overlay_session.h"

#include <dwmapi.h>

#include <array>

namespace airshot::overlay_detail {
namespace {

constexpr std::array<float, 13> kTextSizes{12.0F, 14.0F, 16.0F, 18.0F, 20.0F, 24.0F, 28.0F,
                                           32.0F, 36.0F, 48.0F, 64.0F, 72.0F, 96.0F};
constexpr int kTextSizeDropdownRowHeight = 32;

bool tool_uses_points(Tool tool) {
    return tool == Tool::pen || tool_is_privacy_effect(tool) ||
           tool == Tool::highlight || tool == Tool::eraser;
}

double distance_to_segment(POINT point, POINT start, POINT end) {
    const double dx = static_cast<double>(end.x - start.x);
    const double dy = static_cast<double>(end.y - start.y);
    if (dx == 0.0 && dy == 0.0) {
        return std::hypot(static_cast<double>(point.x - start.x), static_cast<double>(point.y - start.y));
    }
    double t = (static_cast<double>(point.x - start.x) * dx + static_cast<double>(point.y - start.y) * dy) /
               (dx * dx + dy * dy);
    t = std::clamp(t, 0.0, 1.0);
    const double nearest_x = static_cast<double>(start.x) + t * dx;
    const double nearest_y = static_cast<double>(start.y) + t * dy;
    return std::hypot(static_cast<double>(point.x) - nearest_x, static_cast<double>(point.y) - nearest_y);
}

bool near_polyline(POINT point, const std::vector<POINT>& points, double threshold) {
    if (points.empty()) {
        return false;
    }
    if (points.size() == 1) {
        return std::hypot(static_cast<double>(point.x - points.front().x), static_cast<double>(point.y - points.front().y)) <= threshold;
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (distance_to_segment(point, points[index - 1], points[index]) <= threshold) {
            return true;
        }
    }
    return false;
}

RectI inflated(RectI rect, int amount) {
    rect.left -= amount;
    rect.top -= amount;
    rect.right += amount;
    rect.bottom += amount;
    return rect;
}

bool hit_annotation(
    const Annotation& annotation,
    POINT point,
    double additional_radius = 0.0) {
    const double safe_additional_radius =
        std::isfinite(additional_radius)
            ? std::max(0.0, additional_radius)
            : 0.0;
    const double threshold =
        std::max(
            8.0,
            static_cast<double>(annotation_visual_radius(annotation)) + 4.0) +
        safe_additional_radius;
    switch (annotation.tool) {
        case Tool::rectangle:
        case Tool::ellipse:
            return shape_annotation_hit_test(
                annotation,
                point,
                6.0 + safe_additional_radius);
        case Tool::line:
        case Tool::arrow:
            return distance_to_segment(point, annotation.start, annotation.end) <= threshold;
        case Tool::pen:
        case Tool::highlight:
            return near_polyline(point, annotation.points, threshold);
        case Tool::mosaic:
        case Tool::blur:
        case Tool::redact:
            return privacy_annotation_hit_test(
                annotation,
                point,
                safe_additional_radius);
        case Tool::text:
            return inflated(
                annotation_bounds(annotation),
                static_cast<int>(std::ceil(
                    4.0 + safe_additional_radius))).contains(point);
        case Tool::serial:
            return inflated(
                annotation_bounds(annotation),
                static_cast<int>(std::ceil(
                    safe_additional_radius))).contains(point);
        default:
            return false;
    }
}

bool tool_supports_width(Tool tool) {
    return tool == Tool::rectangle || tool == Tool::ellipse || tool == Tool::line || tool == Tool::arrow ||
           tool == Tool::pen || tool == Tool::mosaic || tool == Tool::highlight || tool == Tool::serial ||
           tool == Tool::blur || tool == Tool::redact || tool == Tool::eraser;
}

int slider_value_from_point(
    const ToolbarButton& button,
    POINT point,
    OverlayUiMetrics ui) {
    const int track_left =
        button.bounds.left + ui.px(kSubToolbarSliderTrackLeftDip);
    const int track_right =
        button.bounds.right - ui.px(kSubToolbarSliderTrackRightInsetDip);
    const int track_width = std::max(1, track_right - track_left);
    const int raw = static_cast<int>(std::lround((point.x - track_left) * 100.0 / track_width));
    return std::clamp(raw, 0, 100);
}

bool slider_track_contains(
    const ToolbarButton& button,
    POINT point,
    OverlayUiMetrics ui) {
    const int hit_slop = ui.px(kSubToolbarSliderHitSlopDip);
    const int track_left =
        button.bounds.left + ui.px(kSubToolbarSliderTrackLeftDip);
    const int track_right =
        button.bounds.right - ui.px(kSubToolbarSliderTrackRightInsetDip);
    const int track_y =
        button.bounds.top + button.bounds.height() / 2;
    return point.x >= track_left - hit_slop &&
           point.x <= track_right + hit_slop &&
           std::abs(point.y - track_y) <= hit_slop;
}

bool shortcut_triggered(std::wstring_view shortcut, WPARAM key) {
    const auto hotkey = parse_hotkey(shortcut);
    if (!hotkey || hotkey->virtual_key != key) {
        return false;
    }

    const bool actual_ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool actual_alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool actual_shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool actual_win =
        (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
    return ((hotkey->modifiers & MOD_CONTROL) != 0) == actual_ctrl &&
           ((hotkey->modifiers & MOD_ALT) != 0) == actual_alt &&
           ((hotkey->modifiers & MOD_SHIFT) != 0) == actual_shift &&
           ((hotkey->modifiers & MOD_WIN) != 0) == actual_win;
}

}  // namespace

OverlaySession::OverlaySession(RegionRequest request, RegionCaptureCompletion completion)
    : request_(std::move(request)),
      completion_(std::move(completion)),
      tool_styles_(request_.config.annotation_highlight_alpha) {
    custom_color_ = parse_hex_color(request_.config.custom_color, RGB(128, 0, 255));
    active_highlight_alpha_ = std::clamp(request_.config.annotation_highlight_alpha, 24, 192);
    load_persisted_tool_styles();
    const bool mosaic_hidden = annotation_tool_hidden(
        request_.config.annotation_hidden_tools,
        L"mosaic");
    const bool blur_hidden = annotation_tool_hidden(
        request_.config.annotation_hidden_tools,
        L"blur");
    const bool redaction_hidden = annotation_tool_hidden(
        request_.config.annotation_hidden_tools,
        L"redact");
    mosaic_is_blur_ = mosaic_hidden && !blur_hidden;
    privacy_is_redaction_ =
        mosaic_hidden && blur_hidden && !redaction_hidden;
}

OverlaySession::~OverlaySession() {
    done_ = true;
    stop_ocr_worker();
    destroy_scroll_windows();
    if (prompt_window_ && IsWindow(prompt_window_)) {
        DestroyWindow(prompt_window_);
    }
    prompt_window_ = nullptr;
    destroy_windows();
    release_overlay_factories();
}

bool OverlaySession::start() {
    if (started_) {
        return !done_;
    }
    started_ = true;
    monitors_ = capture_monitors(request_.config.capture_cursor);
    if (monitors_.empty() || std::ranges::any_of(monitors_, [](const auto& monitor) { return monitor.bitmap.empty(); })) {
        finish({ExitCode::operation_failed, std::wstring(strings::capture_failed)});
        return false;
    }
    std::vector<DisplayMonitorGeometry> topology;
    topology.reserve(monitors_.size());
    for (const auto& monitor : monitors_) {
        topology.push_back({
            monitor.bounds,
            monitor.primary,
            monitor.device_name});
    }
    capture_topology_signature_ =
        display_topology_signature(topology);
    if (capture_topology_signature_.empty()) {
        finish({ExitCode::operation_failed, std::wstring(strings::capture_failed)});
        return false;
    }
    window_candidates_ = enumerate_window_candidates();
    virtual_bounds_ = monitors_.front().bounds;
    for (const auto& monitor : monitors_) {
        virtual_bounds_.left = std::min(virtual_bounds_.left, monitor.bounds.left);
        virtual_bounds_.top = std::min(virtual_bounds_.top, monitor.bounds.top);
        virtual_bounds_.right = std::max(virtual_bounds_.right, monitor.bounds.right);
        virtual_bounds_.bottom = std::max(virtual_bounds_.bottom, monitor.bounds.bottom);
    }

    for (auto& monitor : monitors_) {
        auto window = std::make_unique<OverlayWindow>(*this, monitor);
        if (!window->create()) {
            finish({ExitCode::operation_failed, L"无法创建截图窗口。"});
            break;
        }
        windows_.push_back(std::move(window));
    }
    if (!windows_.empty() && !done_) {
        for (const auto& window : windows_) {
            ShowWindow(window->hwnd(), SW_SHOWNOACTIVATE);
        }
        SetForegroundWindow(windows_.front()->hwnd());
        SetFocus(windows_.front()->hwnd());
    }
    return !done_;
}

RegionResult OverlaySession::run() {
    start();
    MSG message{};
    while (!done_) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            if (!done_) {
                finish({ExitCode::user_cancelled, L"已取消。"});
            }
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    destroy_windows();
    return std::move(result_);
}

void OverlaySession::cancel() {
    (void)settle_active_interaction(
        nullptr,
        InteractionSettleMode::cancel);
    finish({ExitCode::user_cancelled, L"已取消。"});
}

void OverlaySession::destroy_windows() {
    for (auto& window : windows_) {
        window->destroy();
    }
    windows_.clear();
}

void OverlaySession::release_capture_if_owned(HWND source) noexcept {
    const HWND captured = GetCapture();
    if (!captured) {
        return;
    }
    const bool session_owns_capture =
        captured == source ||
        std::ranges::any_of(
            windows_,
            [captured](const auto& window) {
                return window && window->hwnd() == captured;
            });
    if (!session_owns_capture) {
        return;
    }
    releasing_capture_ = true;
    ReleaseCapture();
    releasing_capture_ = false;
}

bool OverlaySession::settle_active_interaction(
    HWND source,
    InteractionSettleMode mode) {
    const bool had_interaction =
        toolbar_press_.active() || dragging_toolbar_ || dragging_slider_ ||
        drawing_annotation_ || dragging_selection_;
    if (!had_interaction) {
        return false;
    }

    bool completed_annotation_gesture = false;
    if (mode == InteractionSettleMode::cancel) {
        if (dragging_toolbar_) {
            toolbar_custom_origin_ = toolbar_drag_origin_;
        }
        if (dragging_slider_) {
            if (dragging_slider_id_ == L"mosaic_strength_slider") {
                mosaic_strength_ = dragging_slider_start_value_;
            } else if (
                dragging_slider_id_ == L"watermark_opacity_slider") {
                watermark_opacity_ = dragging_slider_start_value_;
            }
        }
        if (dragging_selection_ &&
            !(current_drag_mode_ == DragMode::annotate &&
              active_tool_ == Tool::select)) {
            if (selection_complete_ && !original_selection_.empty()) {
                selection_ = original_selection_;
            } else if (!selection_complete_) {
                selection_ = {};
            }
        }

        cancel_annotation_transaction();
        if (active_tool_ == Tool::select &&
            annotation_drag_source_idx_ >= 0 &&
            annotation_drag_source_idx_ <
                static_cast<int>(annotations_.size())) {
            selected_annotation_idx_ = annotation_drag_source_idx_;
        }
    } else {
        if (dragging_slider_) {
            commit_annotation_transaction();
        }
        if (drawing_annotation_) {
            normalize_annotation_stroke(preview_);
            if (preview_.tool == Tool::eraser) {
                commit_annotation_transaction();
            } else if (annotation_is_committable(preview_)) {
                record_annotation_change();
                annotations_.push_back(preview_);
            }
            completed_annotation_gesture = true;
        }
        if (dragging_selection_) {
            if (current_drag_mode_ == DragMode::annotate &&
                active_tool_ == Tool::select) {
                commit_annotation_transaction();
            } else {
                RectI settled = selection_.normalized();
                const auto clipped = intersect(settled, virtual_bounds_);
                if (clipped && clipped->width() >= 2 &&
                    clipped->height() >= 2) {
                    selection_ = *clipped;
                } else if (!original_selection_.empty()) {
                    selection_ = original_selection_;
                } else {
                    selection_ = {};
                    selection_complete_ = false;
                }
            }
        }
    }

    reset_annotation_drag_state();
    if (mode == InteractionSettleMode::cancel &&
        active_tool_ == Tool::select && selected_annotation_idx_ != -1) {
        sync_active_style_from_selected();
    }
    if (mode == InteractionSettleMode::cancel) {
        remember_current_style();
    }
    dragging_toolbar_ = false;
    dragging_slider_ = false;
    dragging_slider_id_.clear();
    toolbar_press_.cancel();
    drawing_annotation_ = false;
    dragging_selection_ = false;
    highlight_constraint_active_ = false;
    current_drag_mode_ = DragMode::none;
    preview_ = {};
    release_capture_if_owned(source);
    if (completed_annotation_gesture) {
        finish_annotation();
    }
    build_toolbar();
    build_sub_toolbar();
    invalidate_all();
    return true;
}

bool OverlaySession::cancel_active_interaction(HWND source) {
    return settle_active_interaction(
        source,
        InteractionSettleMode::cancel);
}

void OverlaySession::on_capture_lost() {
    if (!releasing_capture_) {
        hovered_button_id_.clear();
        (void)cancel_active_interaction(nullptr);
    }
}

AnnotationHandle OverlaySession::selected_annotation_handle_at(
    POINT point) const noexcept {
    if (!selection_complete_ ||
        active_tool_ != Tool::select ||
        selected_annotation_idx_ < 0 ||
        selected_annotation_idx_ >=
            static_cast<int>(annotations_.size())) {
        return AnnotationHandle::none;
    }
    const POINT relative{
        point.x - selection_.left,
        point.y - selection_.top,
    };
    return hit_test_annotation_control_handle(
        annotations_[static_cast<std::size_t>(
            selected_annotation_idx_)],
        relative,
        OverlayUiMetrics{ui_dpi_at(point)}.px(9));
}

void OverlaySession::reset_annotation_drag_state() noexcept {
    active_annotation_handle_ = AnnotationHandle::none;
    annotation_drag_source_idx_ = -1;
    clone_annotation_on_drag_ = false;
    annotation_drag_clone_created_ = false;
}

void OverlaySession::duplicate_selected_annotation() {
    if (active_tool_ != Tool::select ||
        selected_annotation_idx_ < 0 ||
        selected_annotation_idx_ >=
            static_cast<int>(annotations_.size()) ||
        annotation_transaction_before_) {
        return;
    }

    Annotation clone =
        annotations_[static_cast<std::size_t>(
            selected_annotation_idx_)];
    const POINT offset = preferred_clone_translation(
        clone,
        12,
        selection_.width(),
        selection_.height());
    if (offset.x == 0 && offset.y == 0) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (clone.tool == Tool::serial) {
        clone.serial = next_serial_number(annotations_);
    }
    translate_annotation(clone, offset.x, offset.y);

    record_annotation_change();
    annotations_.push_back(std::move(clone));
    selected_annotation_idx_ =
        static_cast<int>(annotations_.size()) - 1;
    sync_active_style_from_selected();
    build_toolbar();
    build_sub_toolbar();
    invalidate_all();
}

RectI OverlaySession::dimension_badge_bounds() const noexcept {
    const POINT anchor{selection_.left, selection_.top};
    return selection_size_badge_bounds(
        selection_, virtual_bounds_, ui_dpi_at(anchor));
}

bool OverlaySession::can_edit_selection_size() const noexcept {
    return selection_complete_ && annotations_.empty() &&
           !ocr_running_ && !scroll_capture_ && !dragging_selection_ &&
           !drawing_annotation_ && !dragging_toolbar_ &&
           !dragging_slider_ &&
           !(prompt_window_ && IsWindow(prompt_window_));
}

void OverlaySession::open_selection_size_prompt(HWND source) {
    if (prompt_window_ && IsWindow(prompt_window_)) {
        SetForegroundWindow(prompt_window_);
        return;
    }
    if (!can_edit_selection_size()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    const RectI badge = dimension_badge_bounds();
    const OverlayUiMetrics ui{ui_dpi_at({badge.left, badge.top})};
    const POINT position{badge.left, badge.bottom + ui.px(6)};
    const bool light = should_use_light_theme(request_.config.theme);
    prompt_window_ = overlay_detail::show_selection_size_prompt(
        modal_owner(source),
        position,
        selection_,
        virtual_bounds_,
        selection_size_anchor_,
        selection_aspect_ratio_locked_,
        request_.config.capture_corner_radius,
        light,
        [this](std::optional<SelectionSizeInput> input) {
            prompt_window_ = nullptr;
            if (done_ || !input) {
                return;
            }
            const auto resized = resolve_selection_geometry(
                selection_,
                input->x,
                input->y,
                input->width,
                input->height,
                virtual_bounds_,
                input->anchor);
            if (!resized) {
                MessageBeep(MB_ICONWARNING);
                return;
            }
            selection_ = *resized;
            selection_aspect_ratio_locked_ =
                input->aspect_ratio_locked;
            selection_locked_aspect_ratio_ =
                selection_aspect_ratio_locked_ && selection_.height() > 0
                    ? static_cast<double>(selection_.width()) /
                          static_cast<double>(selection_.height())
                    : 0.0;
            selection_size_anchor_ = input->anchor;
            request_.config.capture_corner_radius =
                input->corner_radius;
            dimension_badge_hovered_ = false;
            build_toolbar();
            build_sub_toolbar();
            invalidate_all();
        });
}

void OverlaySession::refresh_capture(HWND source) {
    if (done_ || windows_.empty()) {
        return;
    }
    if (prompt_window_ && IsWindow(prompt_window_)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    // A refresh is a document-level operation. Cancel an unfinished gesture so
    // a half-drawn stroke or half-resized selection cannot be committed merely
    // because the user asked for a newer desktop frame.
    (void)settle_active_interaction(
        source,
        InteractionSettleMode::cancel);

    const auto restore_windows = [this, source] {
        for (const auto& window : windows_) {
            if (window && window->hwnd() && IsWindow(window->hwnd())) {
                ShowWindow(window->hwnd(), SW_SHOWNOACTIVATE);
                window->invalidate();
            }
        }
        (void)DwmFlush();
        HWND focus_target =
            source && IsWindow(source)
                ? source
                : windows_.front()->hwnd();
        if (focus_target && IsWindow(focus_target)) {
            SetForegroundWindow(focus_target);
            SetFocus(focus_target);
        }
    };

    for (const auto& window : windows_) {
        if (window && window->hwnd() && IsWindow(window->hwnd())) {
            ShowWindow(window->hwnd(), SW_HIDE);
        }
    }
    (void)DwmFlush();

    SnapshotRefreshStatus refresh_status =
        SnapshotRefreshStatus::capture_failed;
    try {
        std::vector<MonitorSnapshot> refreshed =
            capture_monitors(request_.config.capture_cursor);
        refresh_status = validate_snapshot_refresh(monitors_, refreshed);
        if (refresh_status == SnapshotRefreshStatus::compatible) {
            std::vector<WindowCandidate> refreshed_candidates =
                enumerate_window_candidates();
            if (!apply_snapshot_refresh(monitors_, refreshed)) {
                refresh_status = SnapshotRefreshStatus::capture_failed;
            } else {
                window_candidates_ = std::move(refreshed_candidates);
            }
        }
        if (refresh_status == SnapshotRefreshStatus::compatible) {
            for (const auto& window : windows_) {
                window->refresh_snapshot();
            }
            rendered_selection_cache_ = {};
            rendered_selection_cache_revision_ = 0;
            rendered_selection_cache_bounds_ = {};
            effect_preview_cache_ = {};
            effect_preview_cache_revision_ = 0;
            effect_preview_cache_bounds_ = {};
            mark_annotation_visual_changed();

            clicked_window_ = {};
            hover_cycle_anchor_.reset();
            hover_ancestor_offset_ = 0;
            if (!selection_complete_) {
                POINT cursor{};
                GetCursorPos(&cursor);
                cursor_pos_ = cursor;
                const auto candidate = window_candidate_at_point(
                    window_candidates_,
                    cursor,
                    0);
                hover_ = candidate ? candidate->bounds : RectI{};
            }
        }
    } catch (...) {
        refresh_status = SnapshotRefreshStatus::capture_failed;
    }

    restore_windows();
    if (refresh_status == SnapshotRefreshStatus::compatible) {
        return;
    }
    show_output_error(
        source,
        refresh_status == SnapshotRefreshStatus::topology_changed
            ? L"显示器布局已发生变化，当前截图未刷新。请退出后重新截图。"
            : L"屏幕刷新失败，已保留当前截图。请稍后重试。");
}

DragMode OverlaySession::hit_test_drag_mode(POINT point) const {
    if (!selection_complete_) {
        return DragMode::none;
    }
    switch (selected_annotation_handle_at(point)) {
        case AnnotationHandle::top_left:
        case AnnotationHandle::bottom_right:
            return DragMode::top_left;
        case AnnotationHandle::top_right:
        case AnnotationHandle::bottom_left:
            return DragMode::top_right;
        case AnnotationHandle::top:
        case AnnotationHandle::bottom:
            return DragMode::top;
        case AnnotationHandle::left:
        case AnnotationHandle::right:
            return DragMode::left;
        case AnnotationHandle::start_point:
        case AnnotationHandle::end_point:
            return DragMode::annotate;
        case AnnotationHandle::none:
            break;
    }
    // Once annotations exist, changing the capture rectangle would change the
    // underlying pixels without remapping the annotation document. Keep the
    // rectangle stable; editing tools may still operate inside it.
    if (!annotations_.empty()) {
        return selection_.contains(point) &&
                       active_tool_ != Tool::none
                   ? DragMode::annotate
                   : DragMode::none;
    }
    if (selection_.contains(point) &&
        active_tool_ != Tool::none &&
        active_tool_ != Tool::select) {
        return DragMode::annotate;
    }

    const int threshold = OverlayUiMetrics{ui_dpi_at(point)}.px(8);

    bool near_left = std::abs(point.x - selection_.left) <= threshold;
    bool near_right = std::abs(point.x - selection_.right) <= threshold;
    bool near_top = std::abs(point.y - selection_.top) <= threshold;
    bool near_bottom = std::abs(point.y - selection_.bottom) <= threshold;

    if (near_left && near_top) return DragMode::top_left;
    if (near_right && near_top) return DragMode::top_right;
    if (near_left && near_bottom) return DragMode::bottom_left;
    if (near_right && near_bottom) return DragMode::bottom_right;

    bool in_x_range = point.x >= selection_.left - threshold && point.x <= selection_.right + threshold;
    bool in_y_range = point.y >= selection_.top - threshold && point.y <= selection_.bottom + threshold;

    if (near_left && in_y_range) return DragMode::left;
    if (near_right && in_y_range) return DragMode::right;
    if (near_top && in_x_range) return DragMode::top;
    if (near_bottom && in_x_range) return DragMode::bottom;

    if (selection_.contains(point)) {
        if (active_tool_ == Tool::none) {
            return DragMode::move;
        }
        return DragMode::annotate;
    }

    return DragMode::none;
}

bool OverlaySession::erase_annotation_at(POINT relative) {
    return erase_annotations_between(relative, relative);
}

bool OverlaySession::erase_annotations_between(POINT start, POINT end) {
    const bool owns_transaction = !annotation_transaction_active();
    if (owns_transaction) {
        begin_annotation_transaction();
    }
    const EraserSweepResult erased = erase_annotations_with_sweep(
        annotations_,
        start,
        end,
        static_cast<double>(
            tool_visual_radius(Tool::eraser, active_width_)),
        eraser_mode_,
        selected_annotation_idx_);
    if (!erased.changed) {
        if (owns_transaction) {
            commit_annotation_transaction();
        }
        return false;
    }
    selected_annotation_idx_ = erased.selected_annotation_idx;
    record_annotation_change();
    build_toolbar();
    build_sub_toolbar();
    invalidate_all();
    if (owns_transaction) {
        commit_annotation_transaction();
    }
    return true;
}

void OverlaySession::on_mouse_down(HWND source, POINT point, bool right) {
    if (ocr_running_) {
        if (!right) {
            for (const auto& button : toolbar_) {
                if (button.id == L"close" &&
                    button.bounds.contains(point) &&
                    toolbar_press_.begin(button, false)) {
                    hovered_button_id_ = button.id;
                    SetCapture(source);
                    invalidate_all();
                    break;
                }
            }
        }
        return;
    }
    if (prompt_window_ && IsWindow(prompt_window_)) {
        SetForegroundWindow(prompt_window_);
        return;
    }
    if (right) {
        if (cancel_active_interaction(source)) {
            return;
        }
        if (selection_complete_) {
            if (text_size_dropdown_open_) {
                text_size_dropdown_open_ = false;
                text_size_hovered_idx_ = -1;
                invalidate_all();
            } else if (selected_annotation_idx_ != -1) {
                selected_annotation_idx_ = -1;
                build_sub_toolbar();
                invalidate_all();
            } else if (active_tool_ != Tool::none) {
                active_tool_ = Tool::none;
                build_toolbar();
                build_sub_toolbar();
                invalidate_all();
            } else {
                show_quick_menu(source, point);
            }
        } else {
            finish({ExitCode::user_cancelled, L"已取消。"});
        }
        return;
    }
    if (selection_complete_) {
        if (can_edit_selection_size() &&
            dimension_badge_bounds().contains(point)) {
            open_selection_size_prompt(source);
            return;
        }
        if (text_size_dropdown_open_) {
            RectI dropdown_bounds = get_text_size_dropdown_bounds();
            if (dropdown_bounds.contains(point)) {
                int local_y = point.y - dropdown_bounds.top;
                const int row_height =
                    OverlayUiMetrics{ui_dpi_at(point)}.px(
                        kTextSizeDropdownRowHeight);
                int idx = local_y / row_height;
                if (idx >= 0 && idx < static_cast<int>(kTextSizes.size())) {
                    active_text_size_ = kTextSizes[static_cast<std::size_t>(idx)];
                    apply_active_style_to_selected();
                    remember_current_style();
                }
                text_size_dropdown_open_ = false;
                text_size_hovered_idx_ = -1;
                build_sub_toolbar();
                invalidate_all();
                return;
            } else {
                text_size_dropdown_open_ = false;
                text_size_hovered_idx_ = -1;
                invalidate_all();

                bool clicked_dropdown_btn = false;
                for (const auto& button : sub_toolbar_) {
                    if (button.id == L"text_size_btn" && button.bounds.contains(point)) {
                        clicked_dropdown_btn = true;
                        break;
                    }
                }
                if (clicked_dropdown_btn) {
                    return;
                }
            }
        }

        for (const auto& button : toolbar_) {
            if (button.id == L"|") continue;
            if (button.bounds.contains(point)) {
                if (button.id == L"drag") {
                    RectI toolbar_bounds = toolbar_.front().bounds;
                    for (const auto& candidate : toolbar_) {
                        toolbar_bounds.left =
                            std::min(toolbar_bounds.left, candidate.bounds.left);
                        toolbar_bounds.top =
                            std::min(toolbar_bounds.top, candidate.bounds.top);
                    }
                    dragging_toolbar_ = true;
                    toolbar_drag_start_ = point;
                    toolbar_drag_origin_ = {
                        toolbar_bounds.left -
                            OverlayUiMetrics{ui_dpi_at(point)}.px(
                                kToolbarPaddingDip),
                        toolbar_bounds.top -
                            OverlayUiMetrics{ui_dpi_at(point)}.px(
                                kToolbarPaddingDip),
                    };
                    toolbar_custom_origin_ = toolbar_drag_origin_;
                    hovered_button_id_ = L"drag";
                    SetCapture(source);
                    invalidate_all();
                    return;
                }
                if (!button.enabled) {
                    return;
                }
                if (toolbar_press_.begin(button, false)) {
                    hovered_button_id_ = button.id;
                    SetCapture(source);
                    invalidate_all();
                }
                return;
            }
        }
        for (const auto& button : sub_toolbar_) {
            if (button.id == L"|") continue;
            if (button.bounds.contains(point)) {
                if (!button.enabled) {
                    return;
                }
                if (button.id == L"mosaic_strength_slider") {
                    const OverlayUiMetrics ui{ui_dpi_at(point)};
                    if (!slider_track_contains(button, point, ui)) {
                        return;
                    }
                    begin_annotation_transaction();
                    dragging_slider_start_value_ = mosaic_strength_;
                    mosaic_strength_ = slider_value_from_point(button, point, ui);
                    remember_current_style();
                    dragging_slider_ = true;
                    dragging_slider_id_ = button.id;
                    SetCapture(source);
                    apply_active_style_to_selected();
                    invalidate_all();
                    return;
                }
                if (button.id == L"watermark_opacity_slider") {
                    const OverlayUiMetrics ui{ui_dpi_at(point)};
                    if (!slider_track_contains(button, point, ui)) {
                        return;
                    }
                    dragging_slider_start_value_ = watermark_opacity_;
                    watermark_opacity_ = slider_value_from_point(button, point, ui);
                    dragging_slider_ = true;
                    dragging_slider_id_ = button.id;
                    SetCapture(source);
                    invalidate_all();
                    return;
                }
                if (toolbar_press_.begin(button, true)) {
                    hovered_button_id_ = button.id;
                    SetCapture(source);
                    invalidate_all();
                }
                return;
            }
        }

        const AnnotationHandle selected_handle =
            active_tool_ == Tool::select
                ? selected_annotation_handle_at(point)
                : AnnotationHandle::none;
        DragMode mode =
            selected_handle != AnnotationHandle::none
                ? DragMode::annotate
                : hit_test_drag_mode(point);
        if (mode == DragMode::none) {
            return;
        }

        if (mode == DragMode::annotate) {
            POINT relative{point.x - selection_.left, point.y - selection_.top};
            if (active_tool_ == Tool::select) {
                int hit_index = -1;
                if (selected_handle != AnnotationHandle::none &&
                    selected_annotation_idx_ >= 0 &&
                    selected_annotation_idx_ <
                        static_cast<int>(annotations_.size())) {
                    hit_index = selected_annotation_idx_;
                } else {
                    for (int i = static_cast<int>(annotations_.size()) - 1;
                         i >= 0;
                         --i) {
                        if (hit_annotation(annotations_[i], relative)) {
                            hit_index = i;
                            break;
                        }
                    }
                }
                selected_annotation_idx_ = hit_index;
                if (selected_annotation_idx_ != -1) {
                    original_annotation_ =
                        annotations_[static_cast<std::size_t>(
                            selected_annotation_idx_)];
                    reset_annotation_drag_state();
                    active_annotation_handle_ = selected_handle;
                    annotation_drag_source_idx_ = selected_annotation_idx_;
                    clone_annotation_on_drag_ =
                        selected_handle == AnnotationHandle::none &&
                        (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    sync_active_style_from_selected();
                    build_sub_toolbar();
                    begin_annotation_transaction();
                    dragging_selection_ = true;
                    current_drag_mode_ = DragMode::annotate;
                    drag_start_ = point;
                    SetCapture(source);
                    invalidate_all();
                } else {
                    selected_annotation_idx_ = -1;
                    reset_annotation_drag_state();
                    build_sub_toolbar();
                    if (!annotations_.empty()) {
                        dragging_selection_ = false;
                        current_drag_mode_ = DragMode::none;
                        invalidate_all();
                        return;
                    }
                    dragging_selection_ = true;
                    current_drag_mode_ = DragMode::move;
                    drag_start_ = point;
                    original_selection_ = selection_;
                    SetCapture(source);
                    invalidate_all();
                }
                return;
            }
            if (active_tool_ == Tool::text) {
                if (prompt_window_ && IsWindow(prompt_window_)) {
                    SetForegroundWindow(prompt_window_);
                    return;
                }
                bool is_light = should_use_light_theme(request_.config.theme);
                const COLORREF color = active_color_;
                const float text_size = active_text_size_;
                const TextStyle text_style = active_text_style_;
                prompt_window_ = show_text_prompt(
                    source,
                    point,
                    color,
                    text_size,
                    is_light,
                    [this, relative, color, text_size, text_style](std::optional<std::wstring> text) {
                        prompt_window_ = nullptr;
                        if (done_ || !text || text->empty()) {
                            return;
                        }
                        record_annotation_change();
                        Annotation annotation;
                        annotation.tool = Tool::text;
                        annotation.start = relative;
                        annotation.end = relative;
                        annotation.text = std::move(*text);
                        annotation.color = color;
                        annotation.width = text_size;
                        annotation.text_style = text_style;
                        (void)fit_text_annotation_to_canvas(
                            annotation,
                            request_.config,
                            selection_.width(),
                            selection_.height());
                        annotations_.push_back(std::move(annotation));
                        finish_annotation();
                    },
                    {},
                    request_.config.text_font_family,
                    request_.config.text_font_bold,
                    request_.config.text_font_italic,
                    text_style);
                return;
            }
            if (active_tool_ == Tool::watermark) {
                apply_watermark();
                return;
            }
            if (active_tool_ == Tool::serial) {
                record_annotation_change();
                annotations_.push_back({Tool::serial,
                                        relative,
                                        relative,
                                        {},
                                        {},
                                        active_color_,
                                        active_width_,
                                        255,
                                        next_serial_number(annotations_)});
                finish_annotation();
                return;
            }
            if (active_tool_ == Tool::eraser) {
                begin_annotation_transaction();
                erase_annotation_at(relative);
                drawing_annotation_ = true;
                preview_ = {active_tool_, relative, relative, {relative}, {}, active_color_, active_width_};
                current_drag_mode_ = DragMode::annotate;
                SetCapture(source);
                return;
            }
            drawing_annotation_ = true;
            annotation_anchor_ = relative;
            preview_ = {active_tool_,
                        relative,
                        relative,
                        {},
                        {},
                        active_color_,
                        active_width_,
                        active_tool_ == Tool::highlight ? active_highlight_alpha_ :
                        ((active_tool_ == Tool::mosaic || active_tool_ == Tool::blur) ? mosaic_strength_ : 255)};
            preview_.fill_style = active_fill_style_;
            preview_.stroke_pattern = active_stroke_pattern_;
            preview_.arrow_head_style = active_arrow_head_style_;
            preview_.rounded_rectangle = active_rectangle_rounded_;
            if (tool_uses_points(active_tool_)) {
                if (!(tool_is_privacy_effect(active_tool_) &&
                      mosaic_is_rect_)) {
                    preview_.points.push_back(relative);
                }
            }
            current_drag_mode_ = DragMode::annotate;
            highlight_constraint_active_ = false;
            mark_preview_visual_changed();
            SetCapture(source);
            return;
        }

        dragging_selection_ = true;
        current_drag_mode_ = mode;
        drag_start_ = point;
        original_selection_ = selection_;
        SetCapture(source);
        return;
    }

    dragging_selection_ = true;
    current_drag_mode_ = DragMode::none;
    drag_start_ = point;
    selection_ = {point.x, point.y, point.x, point.y};
    clicked_window_ = hover_;
    SetCapture(source);
    invalidate_all();
}

void OverlaySession::on_mouse_move(POINT point) {
    cursor_pos_ = point;
    if (toolbar_press_.active()) {
        if (toolbar_press_.update(point)) {
            hovered_button_id_ = toolbar_press_.pointer_inside
                                     ? toolbar_press_.id
                                     : std::wstring{};
            invalidate_all();
        }
        return;
    }
    if (ocr_running_) {
        return;
    }
    const bool badge_hovered =
        can_edit_selection_size() &&
        dimension_badge_bounds().contains(point);
    if (badge_hovered != dimension_badge_hovered_) {
        dimension_badge_hovered_ = badge_hovered;
        invalidate_all();
    }
    if (dragging_toolbar_) {
        toolbar_custom_origin_ = POINT{
            toolbar_drag_origin_.x + point.x - toolbar_drag_start_.x,
            toolbar_drag_origin_.y + point.y - toolbar_drag_start_.y,
        };
        hovered_button_id_ = L"drag";
        build_toolbar();
        invalidate_all();
        return;
    }
    if (dragging_slider_) {
        for (const auto& button : sub_toolbar_) {
            if (button.id != dragging_slider_id_) {
                continue;
            }
            if (button.id == L"mosaic_strength_slider") {
                mosaic_strength_ = slider_value_from_point(
                    button,
                    point,
                    OverlayUiMetrics{ui_dpi_at(point)});
                apply_active_style_to_selected();
                remember_current_style();
            } else if (button.id == L"watermark_opacity_slider") {
                watermark_opacity_ = slider_value_from_point(
                    button,
                    point,
                    OverlayUiMetrics{ui_dpi_at(point)});
            }
            invalidate_all();
            return;
        }
    }
    if (selection_complete_ && text_size_dropdown_open_) {
        RectI dropdown_bounds = get_text_size_dropdown_bounds();
        if (dropdown_bounds.contains(point)) {
            int local_y = point.y - dropdown_bounds.top;
            const int row_height =
                OverlayUiMetrics{ui_dpi_at(point)}.px(
                    kTextSizeDropdownRowHeight);
            int idx = local_y / row_height;
            if (idx >= 0 && idx < static_cast<int>(kTextSizes.size())) {
                if (text_size_hovered_idx_ != idx) {
                    text_size_hovered_idx_ = idx;
                    invalidate_all();
                }
            }
            if (!hovered_button_id_.empty()) {
                hovered_button_id_ = L"";
                invalidate_all();
            }
            return;
        } else {
            if (text_size_hovered_idx_ != -1) {
                text_size_hovered_idx_ = -1;
                invalidate_all();
            }
        }
    }

    if (selection_complete_ && drawing_annotation_) {
        const POINT raw_relative{
            point.x - selection_.left,
            point.y - selection_.top,
        };
        const POINT relative{
            std::clamp(raw_relative.x, 0L, static_cast<long>(selection_.width())),
            std::clamp(raw_relative.y, 0L, static_cast<long>(selection_.height())),
        };
        const POINT previous_eraser_position = preview_.end;
        const bool constrain = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool from_center = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (preview_.tool == Tool::rectangle ||
            preview_.tool == Tool::ellipse ||
            preview_.tool == Tool::line ||
            preview_.tool == Tool::arrow) {
            const AnnotationGeometry geometry =
                fit_annotation_geometry_to_canvas(
                    constrained_annotation_geometry(
                        preview_.tool,
                        annotation_anchor_,
                        relative,
                        constrain,
                        from_center),
                    annotation_anchor_,
                    selection_.width(),
                    selection_.height(),
                    from_center,
                    constrain ||
                        preview_.tool == Tool::line ||
                        preview_.tool == Tool::arrow);
            preview_.start = geometry.start;
            preview_.end = geometry.end;
        } else {
            preview_.end = relative;
        }
        if (preview_.tool == Tool::eraser) {
            // Treat every pointer update as a swept circle rather than a
            // sampled point. A fast drag can therefore cross a sparse, long
            // source segment without tunnelling through it.
            erase_annotations_between(previous_eraser_position, relative);
            invalidate_all();
            return;
        }
        if (preview_.tool == Tool::highlight && constrain) {
            const POINT endpoint =
                orthogonal_endpoint(annotation_anchor_, relative);
            preview_.end = endpoint;
            preview_.points.clear();
            preview_.points.push_back(annotation_anchor_);
            (void)append_stroke_point_bounded(
                preview_.points,
                endpoint,
                0.5);
            highlight_constraint_active_ = true;
            mark_preview_visual_changed();
            invalidate_all();
            return;
        }
        if (preview_.tool == Tool::highlight &&
            highlight_constraint_active_) {
            preview_.points.clear();
            preview_.points.push_back(annotation_anchor_);
            highlight_constraint_active_ = false;
        }
        if (tool_uses_points(preview_.tool) && selection_.contains(point)) {
            if (!(tool_is_privacy_effect(preview_.tool) &&
                  mosaic_is_rect_)) {
                double minimum_distance = 2.0;
                if (preview_.tool == Tool::pen) {
                    minimum_distance = std::clamp(
                        static_cast<double>(preview_.width) * 0.25,
                        1.5,
                        4.0);
                }
                (void)append_stroke_point_bounded(
                    preview_.points,
                    relative,
                    minimum_distance);
            }
        }
        mark_preview_visual_changed();
        invalidate_all();
        return;
    }
    if (dragging_selection_) {
        if (current_drag_mode_ == DragMode::none) {
            const POINT snapped{
                snap_coordinate(point.x, true),
                snap_coordinate(point.y, false),
            };
            selection_ = constrained_selection_rect(
                drag_start_,
                snapped,
                virtual_bounds_,
                (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                (GetKeyState(VK_MENU) & 0x8000) != 0);
        } else {
            int dx = point.x - drag_start_.x;
            int dy = point.y - drag_start_.y;

            if (current_drag_mode_ == DragMode::move) {
                int w = original_selection_.width();
                int h = original_selection_.height();
                int left = original_selection_.left + dx;
                int top = original_selection_.top + dy;

                if (left < virtual_bounds_.left) left = virtual_bounds_.left;
                if (left + w > virtual_bounds_.right) left = virtual_bounds_.right - w;
                if (top < virtual_bounds_.top) top = virtual_bounds_.top;
                if (top + h > virtual_bounds_.bottom) top = virtual_bounds_.bottom - h;

                selection_ = {left, top, left + w, top + h};
            } else if (
                current_drag_mode_ == DragMode::annotate &&
                active_tool_ == Tool::select &&
                selected_annotation_idx_ != -1) {
                if (active_annotation_handle_ !=
                    AnnotationHandle::none) {
                    const POINT relative{
                        point.x - selection_.left,
                        point.y - selection_.top,
                    };
                    Annotation updated = original_annotation_;
                    if (original_annotation_.tool == Tool::text) {
                        const TextAnnotationResizePlan plan =
                            plan_text_annotation_resize(
                                original_annotation_,
                                active_annotation_handle_,
                                relative,
                                selection_.width(),
                                selection_.height());
                        if (plan.valid) {
                            updated.start = plan.start;
                            updated.end = plan.start;
                            updated.width = plan.font_size_px;
                            updated.text_box_width_px = plan.box_width_px;
                            if (refresh_text_annotation_bounds(
                                    updated,
                                    request_.config)) {
                                const POINT correction =
                                    text_resize_anchor_delta(
                                        plan,
                                        updated.measured_text_layout_bounds);
                                translate_annotation(
                                    updated,
                                    correction.x,
                                    correction.y);
                                (void)fit_text_annotation_to_canvas(
                                    updated,
                                    request_.config,
                                    selection_.width(),
                                    selection_.height());
                            } else {
                                updated = original_annotation_;
                            }
                        }
                    } else {
                        updated = resize_annotation_from_handle(
                            original_annotation_,
                            active_annotation_handle_,
                            relative,
                            selection_.width(),
                            selection_.height(),
                            (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                            (GetKeyState(VK_MENU) & 0x8000) != 0);
                    }
                    if (!annotation_geometry_equal(
                            updated,
                            annotations_[static_cast<std::size_t>(
                                selected_annotation_idx_)])) {
                        mark_annotation_transaction_changed();
                        annotations_[static_cast<std::size_t>(
                            selected_annotation_idx_)] = updated;
                        if (updated.tool == Tool::text) {
                            active_text_size_ = updated.width;
                        }
                    }
                } else {
                    const POINT clamped_delta =
                        clamp_annotation_translation(
                            original_annotation_,
                            dx,
                            dy,
                            selection_.width(),
                            selection_.height());
                    if (clone_annotation_on_drag_ &&
                        !annotation_drag_clone_created_ &&
                        (std::abs(dx) + std::abs(dy) >= 2) &&
                        (clamped_delta.x != 0 ||
                         clamped_delta.y != 0)) {
                        Annotation clone = original_annotation_;
                        if (clone.tool == Tool::serial) {
                            clone.serial =
                                next_serial_number(annotations_);
                        }
                        annotations_.push_back(std::move(clone));
                        selected_annotation_idx_ =
                            static_cast<int>(annotations_.size()) - 1;
                        annotation_drag_clone_created_ = true;
                        mark_annotation_transaction_changed();
                    }

                    if (!clone_annotation_on_drag_ ||
                        annotation_drag_clone_created_) {
                        Annotation& selected =
                            annotations_[static_cast<std::size_t>(
                                selected_annotation_idx_)];
                        if (clamped_delta.x != 0 ||
                            clamped_delta.y != 0) {
                            mark_annotation_transaction_changed();
                        }
                        selected = original_annotation_;
                        if (annotation_drag_clone_created_ &&
                            selected.tool == Tool::serial) {
                            selected.serial =
                                next_serial_number(
                                    *annotation_transaction_before_);
                        }
                        translate_annotation(
                            selected,
                            clamped_delta.x,
                            clamped_delta.y);
                    }
                }
            } else {
                const bool corner_drag =
                    current_drag_mode_ == DragMode::top_left ||
                    current_drag_mode_ == DragMode::top_right ||
                    current_drag_mode_ == DragMode::bottom_right ||
                    current_drag_mode_ == DragMode::bottom_left;
                if (corner_drag) {
                    selection_ = resize_selection_from_corner(
                        original_selection_,
                        current_drag_mode_,
                        POINT{
                            snap_coordinate(point.x, true),
                            snap_coordinate(point.y, false),
                        },
                        virtual_bounds_,
                        selection_aspect_ratio_locked_ ||
                            (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                        (GetKeyState(VK_MENU) & 0x8000) != 0,
                        selection_aspect_ratio_locked_
                            ? selection_locked_aspect_ratio_
                            : 0.0);
                } else {
                    POINT snapped = point;
                    switch (current_drag_mode_) {
                        case DragMode::top:
                            snapped.y = snap_coordinate(
                                original_selection_.top + dy,
                                false);
                            break;
                        case DragMode::right:
                            snapped.x = snap_coordinate(
                                original_selection_.right + dx,
                                true);
                            break;
                        case DragMode::bottom:
                            snapped.y = snap_coordinate(
                                original_selection_.bottom + dy,
                                false);
                            break;
                        case DragMode::left:
                            snapped.x = snap_coordinate(
                                original_selection_.left + dx,
                                true);
                            break;
                        default:
                            break;
                    }
                    selection_ = resize_selection_from_edge(
                        original_selection_,
                        current_drag_mode_,
                        snapped,
                        virtual_bounds_,
                        selection_aspect_ratio_locked_ ||
                            (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                        selection_aspect_ratio_locked_
                            ? selection_locked_aspect_ratio_
                            : 0.0);
                }
            }
        }
        if (!annotations_.empty() &&
            !(current_drag_mode_ == DragMode::annotate &&
              active_tool_ == Tool::select &&
              selected_annotation_idx_ != -1)) {
            mark_annotation_visual_changed();
        }
        build_toolbar();
        invalidate_all();
        return;
    }
    if (selection_complete_) {
        std::wstring current_hovered;
        for (const auto& button : toolbar_) {
            if (button.id == L"|") continue;
            if (button.bounds.contains(point)) {
                current_hovered = button.id;
                break;
            }
        }
        if (current_hovered.empty()) {
            for (const auto& button : sub_toolbar_) {
                if (button.id == L"|") continue;
                if (button.bounds.contains(point)) {
                    current_hovered = button.id;
                    break;
                }
            }
        }
        if (current_hovered != hovered_button_id_) {
            hovered_button_id_ = current_hovered;
            invalidate_all();
        }
        return;
    }
    if (!hover_cycle_anchor_ ||
        std::abs(point.x - hover_cycle_anchor_->x) > 2 ||
        std::abs(point.y - hover_cycle_anchor_->y) > 2) {
        hover_cycle_anchor_ = point;
        hover_ancestor_offset_ = 0;
    }
    const auto candidate = window_candidate_at_point(
        window_candidates_,
        point,
        hover_ancestor_offset_);
    hover_ = candidate ? candidate->bounds : RectI{};
    invalidate_all();
}

void OverlaySession::on_mouse_up(HWND source, POINT point) {
    if (toolbar_press_.active()) {
        const auto activation = toolbar_press_.release(point);
        release_capture_if_owned(source);
        hovered_button_id_ = activation ? activation->id : std::wstring{};
        invalidate_all();
        if (activation) {
            if (activation->from_sub_toolbar) {
                invoke_sub(activation->id, source);
                invalidate_all();
            } else {
                invoke(activation->id, source);
            }
        }
        return;
    }
    if (ocr_running_) {
        return;
    }
    release_capture_if_owned(source);
    if (dragging_toolbar_) {
        dragging_toolbar_ = false;
        hovered_button_id_ = L"drag";
        invalidate_all();
        return;
    }
    if (dragging_slider_) {
        dragging_slider_ = false;
        dragging_slider_id_.clear();
        commit_annotation_transaction();
        build_toolbar();
        build_sub_toolbar();
        invalidate_all();
        return;
    }
    if (selection_complete_ && drawing_annotation_) {
        if (preview_.tool == Tool::eraser) {
            const POINT final_relative{
                std::clamp(
                    point.x - selection_.left,
                    0L,
                    static_cast<long>(selection_.width())),
                std::clamp(
                    point.y - selection_.top,
                    0L,
                    static_cast<long>(selection_.height())),
            };
            erase_annotations_between(preview_.end, final_relative);
            preview_.end = final_relative;
        }
        drawing_annotation_ = false;
        highlight_constraint_active_ = false;
        normalize_annotation_stroke(preview_);
        if (preview_.tool != Tool::eraser &&
            annotation_is_committable(preview_)) {
            record_annotation_change();
            annotations_.push_back(preview_);
        } else if (preview_.tool == Tool::eraser) {
            commit_annotation_transaction();
        }
        preview_ = {};
        current_drag_mode_ = DragMode::none;
        finish_annotation();
        return;
    }
    if (!dragging_selection_) {
        return;
    }
    dragging_selection_ = false;
    if (current_drag_mode_ == DragMode::annotate && active_tool_ == Tool::select) {
        commit_annotation_transaction();
        reset_annotation_drag_state();
        current_drag_mode_ = DragMode::none;
        build_toolbar();
        build_sub_toolbar();
        invalidate_all();
        return;
    }

    if (current_drag_mode_ == DragMode::none) {
        const int distance = std::abs(point.x - drag_start_.x) + std::abs(point.y - drag_start_.y);
        if (distance <= 4 && !clicked_window_.empty()) {
            selection_ = clicked_window_;
        }
    }

    selection_ = selection_.normalized();
    current_drag_mode_ = DragMode::none;

    const auto clipped = intersect(selection_, virtual_bounds_);
    if (!clipped || clipped->width() < 2 || clipped->height() < 2) {
        selection_ = {};
        selection_complete_ = false;
        invalidate_all();
        return;
    }
    selection_ = *clipped;

    if (!selection_complete_) {
        selection_complete_ = true;
        build_toolbar();
        invalidate_all();

        if (request_.action == RegionAction::clipboard) {
            complete_clipboard();
        } else if (request_.action == RegionAction::file) {
            complete_file(request_.path, nullptr);
        } else if (request_.action == RegionAction::ocr) {
            complete_ocr();
        }
    } else {
        build_toolbar();
        invalidate_all();
    }
}

void OverlaySession::on_double_click(HWND source, POINT point) {
    if (ocr_running_) {
        return;
    }
    if (!selection_complete_ ||
        !selection_.contains(point) ||
        is_over_toolbar(point)) {
        return;
    }
    if (active_tool_ == Tool::select &&
        selected_annotation_idx_ >= 0 &&
        selected_annotation_idx_ < static_cast<int>(annotations_.size()) &&
        annotations_[static_cast<std::size_t>(selected_annotation_idx_)].tool ==
            Tool::text) {
        edit_selected_text(source);
        return;
    }
    if (active_tool_ == Tool::none) {
        HWND owner = windows_.empty() ? nullptr : windows_.front()->hwnd();
        complete_default(owner);
    }
}

void OverlaySession::on_key_down(
    HWND source,
    WPARAM key,
    bool is_auto_repeat) {
    if (scroll_capture_) {
        switch (scroll_keyboard_command(key)) {
            case ScrollKeyboardCommand::toggle_pause:
                toggle_scroll_pause();
                return;
            case ScrollKeyboardCommand::finish:
                finish_scroll_capture(false);
                return;
            case ScrollKeyboardCommand::cancel:
                finish_scroll_capture(true);
                return;
            case ScrollKeyboardCommand::none:
                return;
        }
    }
    if (ocr_running_) {
        if (key == VK_ESCAPE) {
            cancel_ocr();
        }
        return;
    }
    if (key == VK_ESCAPE) {
        if (text_size_dropdown_open_) {
            text_size_dropdown_open_ = false;
            text_size_hovered_idx_ = -1;
            invalidate_all();
            return;
        }
        if (cancel_active_interaction(source)) {
            return;
        }
        if (selection_complete_ && selected_annotation_idx_ != -1) {
            selected_annotation_idx_ = -1;
            build_sub_toolbar();
            invalidate_all();
            return;
        }
        if (selection_complete_ && active_tool_ != Tool::none) {
            active_tool_ = Tool::none;
            build_toolbar();
            build_sub_toolbar();
            invalidate_all();
            return;
        }
        finish({ExitCode::user_cancelled, L"已取消。"});
        return;
    }
    if (key == VK_F5) {
        if (!is_auto_repeat) {
            refresh_capture(source);
        }
        return;
    }

    // Before selection is complete (magnifier is showing): color picking mode
    if (!selection_complete_) {
        // C key: copy color under cursor to clipboard and exit
        if (key == 'C') {
            POINT cursor_pos{};
            GetCursorPos(&cursor_pos);
            COLORREF color = get_pixel_color(cursor_pos.x, cursor_pos.y);
            std::wstring color_str;
            if (color_format_hex_) {
                color_str = format_hex_color(color);
            } else {
                color_str = std::format(L"rgb({}, {}, {})", GetRValue(color), GetGValue(color), GetBValue(color));
            }
            std::wstring error;
            if (!copy_text_to_clipboard(color_str, &error)) {
                finish({ExitCode::operation_failed, std::move(error)});
                return;
            }

            custom_color_ = color;
            request_.config.custom_color = format_hex_color(color);

            RegionResult result{
                ExitCode::success,
                std::format(L"已复制颜色 {} 到剪贴板。", color_str)};
            result.action = RegionAction::clipboard;
            finish(std::move(result));
            return;
        }
        // Shift key: toggle between Hex and RGB display format in magnifier
        if (key == VK_SHIFT) {
            color_format_hex_ = !color_format_hex_;
            invalidate_all();
            return;
        }
        return;
    }

    // Editing commands take precedence over user-defined tool shortcuts.
    if (shortcut_triggered(
            capture_editor_shortcuts::precision_size,
            key)) {
        open_selection_size_prompt(source);
        return;
    }

    if (active_tool_ == Tool::select &&
        selected_annotation_idx_ >= 0 &&
        selected_annotation_idx_ <
            static_cast<int>(annotations_.size()) &&
        shortcut_triggered(capture_editor_shortcuts::duplicate, key)) {
        duplicate_selected_annotation();
        return;
    }

    if (active_tool_ == Tool::select &&
        selected_annotation_idx_ >= 0 &&
        selected_annotation_idx_ < static_cast<int>(annotations_.size()) &&
        (key == VK_UP || key == VK_DOWN ||
         key == VK_LEFT || key == VK_RIGHT)) {
        const int step =
            (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 10 : 1;
        int delta_x = 0;
        int delta_y = 0;
        if (key == VK_LEFT) delta_x = -step;
        else if (key == VK_RIGHT) delta_x = step;
        else if (key == VK_UP) delta_y = -step;
        else if (key == VK_DOWN) delta_y = step;

        Annotation& selected =
            annotations_[static_cast<std::size_t>(selected_annotation_idx_)];
        const POINT clamped_delta =
            clamp_annotation_translation(
                selected,
                delta_x,
                delta_y,
                selection_.width(),
                selection_.height());
        if (clamped_delta.x != 0 || clamped_delta.y != 0) {
            record_annotation_change();
            translate_annotation(
                selected,
                clamped_delta.x,
                clamped_delta.y);
            build_toolbar();
            build_sub_toolbar();
            invalidate_all();
        }
        return;
    }

    if (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT) {
        // Mouse resizing is already locked once annotations exist. Keep the
        // keyboard path consistent so the capture pixels cannot move under a
        // vector document that remains in selection-relative coordinates.
        if (!annotations_.empty()) {
            return;
        }
        const bool control_down =
            (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift_down =
            (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const SelectionResizeDirection direction =
            key == VK_LEFT
                ? SelectionResizeDirection::left
                : key == VK_RIGHT
                      ? SelectionResizeDirection::right
                      : key == VK_UP
                            ? SelectionResizeDirection::up
                            : SelectionResizeDirection::down;
        if (control_down != shift_down) {
            if (selection_aspect_ratio_locked_) {
                DragMode edge = DragMode::none;
                POINT cursor{};
                switch (direction) {
                    case SelectionResizeDirection::left:
                        edge = control_down ? DragMode::left : DragMode::right;
                        cursor = {
                            control_down ? selection_.left - 1
                                         : selection_.right - 1,
                            selection_.top};
                        break;
                    case SelectionResizeDirection::right:
                        edge = control_down ? DragMode::right : DragMode::left;
                        cursor = {
                            control_down ? selection_.right + 1
                                         : selection_.left + 1,
                            selection_.top};
                        break;
                    case SelectionResizeDirection::up:
                        edge = control_down ? DragMode::top : DragMode::bottom;
                        cursor = {
                            selection_.left,
                            control_down ? selection_.top - 1
                                         : selection_.bottom - 1};
                        break;
                    case SelectionResizeDirection::down:
                        edge = control_down ? DragMode::bottom : DragMode::top;
                        cursor = {
                            selection_.left,
                            control_down ? selection_.bottom + 1
                                         : selection_.top + 1};
                        break;
                }
                selection_ = resize_selection_from_edge(
                    selection_,
                    edge,
                    cursor,
                    virtual_bounds_,
                    true,
                    selection_locked_aspect_ratio_);
            } else {
                selection_ = resize_selection_one_pixel(
                    selection_,
                    virtual_bounds_,
                    direction,
                    control_down);
            }
            build_toolbar();
            build_sub_toolbar();
            invalidate_all();
            return;
        }
        if (control_down && shift_down) {
            return;
        }

        int delta_x = 0;
        int delta_y = 0;
        if (key == VK_LEFT) delta_x = -1;
        else if (key == VK_RIGHT) delta_x = 1;
        else if (key == VK_UP) delta_y = -1;
        else if (key == VK_DOWN) delta_y = 1;
        selection_ = translate_selection_within_bounds(
            selection_,
            delta_x,
            delta_y,
            virtual_bounds_);
        build_toolbar();
        build_sub_toolbar();
        invalidate_all();
        return;
    }

    // Spacebar to show quick menu
    if (key == VK_SPACE) {
        POINT pt{};
        GetCursorPos(&pt);
        show_quick_menu(source, pt);
        return;
    }

    if ((key == VK_DELETE || key == VK_BACK) &&
        (GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
        !annotations_.empty()) {
        record_annotation_change();
        annotations_.clear();
        selected_annotation_idx_ = -1;
        build_toolbar();
        build_sub_toolbar();
        invalidate_all();
        return;
    }

    // Delete/Backspace key to delete selected annotation
    if (active_tool_ == Tool::select && selected_annotation_idx_ != -1) {
        if (key == VK_DELETE || key == VK_BACK) {
            record_annotation_change();
            annotations_.erase(annotations_.begin() + selected_annotation_idx_);
            renumber_serial_annotations(annotations_);
            selected_annotation_idx_ = -1;
            build_toolbar();
            invalidate_all();
            return;
        }
    }

    if (shortcut_triggered(capture_editor_shortcuts::copy, key)) {
        complete_clipboard();
        return;
    }
    if (shortcut_triggered(capture_editor_shortcuts::undo, key)) {
        undo();
        return;
    }
    if (shortcut_triggered(capture_editor_shortcuts::redo, key)) {
        redo();
        return;
    }
    if (shortcut_triggered(
            capture_editor_shortcuts::redo_alternate,
            key)) {
        redo();
        return;
    }
    if (request_.config.ocr_enabled &&
        shortcut_triggered(request_.config.capture_ocr_shortcut, key)) {
        complete_ocr();
        return;
    }
    if (key == VK_RETURN) {
        complete_default(source);
        return;
    }
    if (shortcut_triggered(capture_editor_shortcuts::save, key)) {
        complete_file({}, source);
        return;
    }

    Tool target_tool = Tool::none;
    const auto available =
        [&](std::wstring_view id) {
            return request_.config.annotation_enabled &&
                   !annotation_tool_hidden(
                       request_.config.annotation_hidden_tools,
                       id);
        };
    if (available(L"select") &&
        shortcut_triggered(request_.config.tool_shortcut_select, key)) {
        target_tool = Tool::select;
    } else if (available(L"rect") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_rectangle,
                   key)) {
        target_tool = Tool::rectangle;
    } else if (available(L"ellipse") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_ellipse,
                   key)) {
        target_tool = Tool::ellipse;
    } else if (available(L"line") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_line,
                   key)) {
        target_tool = Tool::line;
    } else if (available(L"arrow") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_arrow,
                   key)) {
        target_tool = Tool::arrow;
    } else if (available(L"pen") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_pen,
                   key)) {
        target_tool = Tool::pen;
    } else if (available(L"mosaic") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_mosaic,
                   key)) {
        target_tool = Tool::mosaic;
    } else if (available(L"blur") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_blur,
                   key)) {
        target_tool = Tool::blur;
    } else if (available(L"highlight") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_highlight,
                   key)) {
        target_tool = Tool::highlight;
    } else if (available(L"text") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_text,
                   key)) {
        target_tool = Tool::text;
    } else if (available(L"serial") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_serial,
                   key)) {
        target_tool = Tool::serial;
    } else if (available(L"eraser") &&
               shortcut_triggered(
                   request_.config.tool_shortcut_eraser,
                   key)) {
        target_tool = Tool::eraser;
    }

    if (target_tool != Tool::none &&
        tool_shortcut_keydown_allowed(is_auto_repeat)) {
        (void)settle_active_interaction(
            source,
            InteractionSettleMode::cancel);
        toggle_active_tool(target_tool);
        build_toolbar();
        build_sub_toolbar();
        invalidate_all();
    }
}

bool OverlaySession::on_system_key_down(
    HWND source,
    WPARAM key,
    bool is_auto_repeat) {
    const std::array<std::wstring_view, 13> configurable_shortcuts{
        request_.config.capture_ocr_shortcut,
        request_.config.tool_shortcut_select,
        request_.config.tool_shortcut_rectangle,
        request_.config.tool_shortcut_ellipse,
        request_.config.tool_shortcut_line,
        request_.config.tool_shortcut_arrow,
        request_.config.tool_shortcut_pen,
        request_.config.tool_shortcut_mosaic,
        request_.config.tool_shortcut_blur,
        request_.config.tool_shortcut_highlight,
        request_.config.tool_shortcut_text,
        request_.config.tool_shortcut_serial,
        request_.config.tool_shortcut_eraser,
    };
    const bool matched = std::ranges::any_of(
        configurable_shortcuts,
        [&](std::wstring_view shortcut) {
            return shortcut_triggered(shortcut, key);
        });
    if (!matched) {
        return false;
    }
    on_key_down(source, key, is_auto_repeat);
    return true;
}

void OverlaySession::invalidate_all() const {
    for (const auto& window : windows_) {
        window->invalidate();
    }
}

[[nodiscard]] RectI OverlaySession::display_selection() const {
    if (!selection_.empty()) {
        return selection_;
    }
    return hover_;
}

RectI OverlaySession::get_text_size_dropdown_bounds() const noexcept {
    for (const auto& button : sub_toolbar_) {
        if (button.id == L"text_size_btn") {
            const POINT anchor{
                button.bounds.left + button.bounds.width() / 2,
                button.bounds.top + button.bounds.height() / 2,
            };
            const OverlayUiMetrics ui{ui_dpi_at(anchor)};
            const int card_width = ui.px(86);
            const int row_height = ui.px(kTextSizeDropdownRowHeight);
            const int card_height =
                static_cast<int>(kTextSizes.size()) * row_height;
            RectI available = virtual_bounds_;
            for (const auto& monitor : monitors_) {
                if (monitor.bounds.contains(anchor)) {
                    available = monitor.bounds;
                    break;
                }
            }
            const int x = card_width >= available.width()
                              ? available.left
                              : std::clamp(button.bounds.left,
                                           available.left,
                                           available.right - card_width);
            int y = button.bounds.bottom + ui.px(4);
            if (y + card_height > available.bottom) {
                y = button.bounds.top - ui.px(4) - card_height;
            }
            if (card_height >= available.height()) {
                y = available.top;
            } else {
                y = std::clamp(y, available.top, available.bottom - card_height);
            }
            return RectI{x, y, x + card_width, y + card_height};
        }
    }
    return {};
}

[[nodiscard]] bool OverlaySession::is_over_toolbar(POINT point) const noexcept {
    if (selection_complete_ && text_size_dropdown_open_) {
        RectI dropdown_bounds = get_text_size_dropdown_bounds();
        if (dropdown_bounds.contains(point)) return true;
    }
    if (!toolbar_.empty()) {
        RectI bounds = toolbar_.front().bounds;
        for (const auto& btn : toolbar_) {
            bounds.left = std::min(bounds.left, btn.bounds.left);
            bounds.top = std::min(bounds.top, btn.bounds.top);
            bounds.right = std::max(bounds.right, btn.bounds.right);
            bounds.bottom = std::max(bounds.bottom, btn.bounds.bottom);
        }
        const OverlayUiMetrics ui{ui_dpi_at(point)};
        bounds.left -= ui.px(8);
        bounds.right += ui.px(8);
        bounds.top -= ui.px(8);
        bounds.bottom += ui.px(8);
        if (bounds.contains(point)) return true;
    }
    if (!sub_toolbar_.empty()) {
        RectI bounds = sub_toolbar_.front().bounds;
        for (const auto& btn : sub_toolbar_) {
            bounds.left = std::min(bounds.left, btn.bounds.left);
            bounds.top = std::min(bounds.top, btn.bounds.top);
            bounds.right = std::max(bounds.right, btn.bounds.right);
            bounds.bottom = std::max(bounds.bottom, btn.bounds.bottom);
        }
        const OverlayUiMetrics ui{ui_dpi_at(point)};
        bounds.left -= ui.px(6);
        bounds.right += ui.px(6);
        bounds.top -= ui.px(6);
        bounds.bottom += ui.px(6);
        if (bounds.contains(point)) return true;
    }
    return false;
}

[[nodiscard]] bool OverlaySession::is_over_toolbar_drag_handle(
    POINT point) const noexcept {
    return std::ranges::any_of(
        toolbar_,
        [point](const ToolbarButton& button) {
            return button.id == L"drag" && button.bounds.contains(point);
        });
}

[[nodiscard]] COLORREF OverlaySession::get_pixel_color(int x, int y) const noexcept {
    for (const auto& monitor : monitors_) {
        if (monitor.bounds.contains({x, y})) {
            int local_x = x - monitor.bounds.left;
            int local_y = y - monitor.bounds.top;
            if (local_x >= 0 && local_x < monitor.bitmap.width &&
                local_y >= 0 && local_y < monitor.bitmap.height) {
                const std::size_t index = static_cast<std::size_t>(local_y * monitor.bitmap.width + local_x) * 4;
                if (index + 2 < monitor.bitmap.pixels.size()) {
                    uint8_t b = monitor.bitmap.pixels[index];
                    uint8_t g = monitor.bitmap.pixels[index + 1];
                    uint8_t r = monitor.bitmap.pixels[index + 2];
                    return RGB(r, g, b);
                }
            }
        }
    }
    return RGB(0, 0, 0);
}

[[nodiscard]] int OverlaySession::snap_coordinate(int value, bool is_x, int threshold) const noexcept {
    int best_snap = value;
    int min_diff = threshold + 1;
    for (const auto& monitor : monitors_) {
        if (is_x) {
            if (std::abs(value - monitor.bounds.left) < min_diff) {
                min_diff = std::abs(value - monitor.bounds.left);
                best_snap = monitor.bounds.left;
            }
            if (std::abs(value - monitor.bounds.right) < min_diff) {
                min_diff = std::abs(value - monitor.bounds.right);
                best_snap = monitor.bounds.right;
            }
        } else {
            if (std::abs(value - monitor.bounds.top) < min_diff) {
                min_diff = std::abs(value - monitor.bounds.top);
                best_snap = monitor.bounds.top;
            }
            if (std::abs(value - monitor.bounds.bottom) < min_diff) {
                min_diff = std::abs(value - monitor.bounds.bottom);
                best_snap = monitor.bounds.bottom;
            }
        }
    }
    for (const auto& candidate : window_candidates_) {
        if (is_x) {
            if (std::abs(value - candidate.bounds.left) < min_diff) {
                min_diff = std::abs(value - candidate.bounds.left);
                best_snap = candidate.bounds.left;
            }
            if (std::abs(value - candidate.bounds.right) < min_diff) {
                min_diff = std::abs(value - candidate.bounds.right);
                best_snap = candidate.bounds.right;
            }
        } else {
            if (std::abs(value - candidate.bounds.top) < min_diff) {
                min_diff = std::abs(value - candidate.bounds.top);
                best_snap = candidate.bounds.top;
            }
            if (std::abs(value - candidate.bounds.bottom) < min_diff) {
                min_diff = std::abs(value - candidate.bounds.bottom);
                best_snap = candidate.bounds.bottom;
            }
        }
    }
    return best_snap;
}

bool OverlaySession::hit_test_annotation(POINT relative) const {
    for (const auto& annotation : annotations_) {
        if (hit_annotation(annotation, relative)) {
            return true;
        }
    }
    return false;
}

void OverlaySession::on_mouse_wheel(short delta) {
    if (ocr_running_) {
        return;
    }
    if (!selection_complete_) {
        if (delta < 0) {
            hover_ancestor_offset_ = std::min<std::size_t>(
                32,
                hover_ancestor_offset_ + 1);
        } else if (delta > 0 && hover_ancestor_offset_ > 0) {
            --hover_ancestor_offset_;
        } else {
            return;
        }
        POINT point = cursor_pos_;
        if (point.x == 0 && point.y == 0) {
            GetCursorPos(&point);
        }
        hover_cycle_anchor_ = point;
        const auto candidate = window_candidate_at_point(
            window_candidates_,
            point,
            hover_ancestor_offset_);
        hover_ = candidate ? candidate->bounds : RectI{};
        invalidate_all();
        return;
    }
    Tool style_tool = active_tool_;
    if (active_tool_ == Tool::select &&
        selected_annotation_idx_ >= 0 &&
        selected_annotation_idx_ < static_cast<int>(annotations_.size())) {
        style_tool =
            annotations_[static_cast<std::size_t>(selected_annotation_idx_)].tool;
    }
    if (style_tool == Tool::text) {
        float step = delta > 0 ? 2.0F : -2.0F;
        active_text_size_ = std::clamp(active_text_size_ + step, 12.0F, 96.0F);
    } else if (tool_supports_width(style_tool)) {
        float step = delta > 0 ? 1.0F : -1.0F;
        active_width_ = std::clamp(active_width_ + step, 1.0F, 50.0F);
    } else {
        return;
    }
    remember_current_style();
    mark_preview_visual_changed();
    apply_active_style_to_selected();
    build_sub_toolbar();
    invalidate_all();
}

void OverlaySession::show_quick_menu(HWND hwnd, POINT pt) {
    if (ocr_running_) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    // Tools Submenu
    HMENU tools_menu = CreatePopupMenu();
    if (!tools_menu) {
        DestroyMenu(menu);
        return;
    }
    bool has_tools = false;
    const auto append_tool =
        [&](UINT_PTR command,
            std::wstring_view id,
            const wchar_t* label) {
            if (!request_.config.annotation_enabled ||
                annotation_tool_hidden(
                    request_.config.annotation_hidden_tools,
                    id)) {
                return;
            }
            AppendMenuW(tools_menu, MF_STRING, command, label);
            has_tools = true;
        };
    append_tool(101, L"select", L"选择工具 (Select)\tSelect");
    append_tool(102, L"rect", L"矩形 (Rectangle)\tRectangle");
    append_tool(103, L"ellipse", L"椭圆 (Ellipse)\tEllipse");
    append_tool(104, L"line", L"直线 (Line)\tLine");
    append_tool(105, L"arrow", L"箭头 (Arrow)\tArrow");
    append_tool(106, L"pen", L"画笔 (Pen)\tPen");
    append_tool(107, L"mosaic", L"马赛克 (Mosaic)\tMosaic");
    append_tool(108, L"blur", L"模糊 (Blur)\tBlur");
    append_tool(114, L"redact", L"实心遮挡 (Solid Redaction)");
    append_tool(109, L"highlight", L"高亮 (Highlight)\tHighlight");
    append_tool(110, L"text", L"文本 (Text)\tText");
    append_tool(111, L"serial", L"序号 (Serial)\tSerial");
    append_tool(112, L"eraser", L"橡皮擦 (Eraser)\tEraser");
    append_tool(113, L"watermark", L"水印 (Watermark)");

    if (has_tools) {
        AppendMenuW(
            menu,
            MF_POPUP,
            reinterpret_cast<UINT_PTR>(tools_menu),
            L"工具 (Tools)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    const bool enter_saves = _wcsicmp(request_.config.default_output.c_str(), L"file") == 0;
    AppendMenuW(menu,
                MF_STRING,
                1,
                enter_saves ? L"复制到剪贴板 (Copy)\tCtrl+C"
                            : L"复制到剪贴板 (Copy)\tCtrl+C / Enter");
    AppendMenuW(menu,
                MF_STRING,
                2,
                enter_saves ? L"保存到文件 (Save)\tCtrl+S / Enter"
                            : L"保存到文件 (Save)\tCtrl+S");
    AppendMenuW(menu, MF_STRING, 3, L"贴图 (Pin)");
    if (request_.config.ocr_enabled) {
        const std::wstring ocr_label =
            std::format(L"屏幕识字 (OCR)\t{}", request_.config.capture_ocr_shortcut);
        AppendMenuW(menu, MF_STRING, 4, ocr_label.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(
        menu,
        MF_STRING |
            (annotation_history_.can_undo() ? MF_ENABLED : MF_GRAYED),
        5,
        L"撤销 (Undo)\tCtrl+Z");
    AppendMenuW(
        menu,
        MF_STRING |
            (annotation_history_.can_redo() ? MF_ENABLED : MF_GRAYED),
        6,
        L"重做 (Redo)\tCtrl+Y / Ctrl+Shift+Z");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 7, L"退出 (Exit)\tEsc");

    // Show menu
    enter_modal();
    int selection = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    if (!has_tools) {
        DestroyMenu(tools_menu);
    }
    DestroyMenu(menu);
    if (done_) {
        leave_modal();
        return;
    }

    if (selection == 1) {
        complete_clipboard();
    } else if (selection == 2) {
        complete_file({}, hwnd);
    } else if (selection == 3) {
        complete_pin();
    } else if (selection == 4) {
        complete_ocr();
    } else if (selection == 5) {
        undo();
    } else if (selection == 6) {
        redo();
    } else if (selection == 7) {
        finish({ExitCode::user_cancelled, L"已取消。"});
    } else if (selection >= 101 && selection <= 114) {
        Tool chosen_tool = Tool::none;
        switch (selection) {
            case 101: chosen_tool = Tool::select; break;
            case 102: chosen_tool = Tool::rectangle; break;
            case 103: chosen_tool = Tool::ellipse; break;
            case 104: chosen_tool = Tool::line; break;
            case 105: chosen_tool = Tool::arrow; break;
            case 106: chosen_tool = Tool::pen; break;
            case 107: chosen_tool = Tool::mosaic; break;
            case 108: chosen_tool = Tool::blur; break;
            case 109: chosen_tool = Tool::highlight; break;
            case 110: chosen_tool = Tool::text; break;
            case 111: chosen_tool = Tool::serial; break;
            case 112: chosen_tool = Tool::eraser; break;
            case 113: chosen_tool = Tool::watermark; break;
            case 114: chosen_tool = Tool::redact; break;
        }
        remember_current_style();
        active_tool_ = chosen_tool;
        load_active_style(chosen_tool);
        selected_annotation_idx_ = -1;
        build_toolbar();
        build_sub_toolbar();
        invalidate_all();
    }
    leave_modal();
}

}  // namespace airshot::overlay_detail
