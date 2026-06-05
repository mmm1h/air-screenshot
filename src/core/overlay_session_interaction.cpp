#include "overlay_session.h"

namespace airshot::overlay_detail {
namespace {

bool tool_uses_points(Tool tool) {
    return tool == Tool::pen || tool == Tool::mosaic || tool == Tool::highlight || tool == Tool::eraser || tool == Tool::blur;
}

bool annotation_has_size(const Annotation& annotation) {
    if (tool_uses_points(annotation.tool)) {
        return annotation.points.size() > 1;
    }
    return annotation.start.x != annotation.end.x || annotation.start.y != annotation.end.y;
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

bool hit_annotation(const Annotation& annotation, POINT point) {
    const double threshold = std::max(8.0, static_cast<double>(annotation.width) + 5.0);
    const RectI bounds{annotation.start.x, annotation.start.y, annotation.end.x, annotation.end.y};
    switch (annotation.tool) {
        case Tool::rectangle:
        case Tool::ellipse:
            return inflated(bounds.normalized(), static_cast<int>(threshold)).contains(point);
        case Tool::line:
        case Tool::arrow:
            return distance_to_segment(point, annotation.start, annotation.end) <= threshold;
        case Tool::pen:
        case Tool::mosaic:
        case Tool::highlight:
        case Tool::blur:
            return near_polyline(point, annotation.points, threshold + ((annotation.tool == Tool::mosaic || annotation.tool == Tool::blur) ? 10.0 : 0.0));
        case Tool::text:
            return RectI{annotation.start.x, annotation.start.y, annotation.start.x + 160, annotation.start.y + 32}.contains(point);
        case Tool::serial:
            return std::hypot(static_cast<double>(point.x - annotation.start.x), static_cast<double>(point.y - annotation.start.y)) <= 18.0;
        default:
            return false;
    }
}

bool tool_supports_width(Tool tool) {
    return tool == Tool::rectangle || tool == Tool::ellipse || tool == Tool::line || tool == Tool::arrow ||
           tool == Tool::pen || tool == Tool::mosaic || tool == Tool::highlight || tool == Tool::eraser ||
           tool == Tool::blur;
}

}  // namespace

OverlaySession::OverlaySession(RegionRequest request) : request_(std::move(request)) {
    custom_color_ = parse_hex_color(request_.config.custom_color, RGB(128, 0, 255));
    active_highlight_alpha_ = std::clamp(request_.config.annotation_highlight_alpha, 24, 192);
}

RegionResult OverlaySession::run() {
    monitors_ = capture_monitors();
    if (monitors_.empty() || std::ranges::any_of(monitors_, [](const auto& monitor) { return monitor.bitmap.empty(); })) {
        return {ExitCode::operation_failed, std::wstring(strings::capture_failed)};
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

    MSG message{};
    while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    for (auto& window : windows_) {
        window->destroy();
    }
    windows_.clear();
    return result_;
}

DragMode OverlaySession::hit_test_drag_mode(POINT point) const {
    if (!selection_complete_) {
        return DragMode::none;
    }

    constexpr int threshold = 8;

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
    for (auto it = annotations_.rbegin(); it != annotations_.rend(); ++it) {
        if (hit_annotation(*it, relative)) {
            redo_.clear();
            annotations_.erase((it + 1).base());
            build_toolbar();
            invalidate_all();
            return true;
        }
    }
    return false;
}

void OverlaySession::on_mouse_down(HWND source, POINT point, bool right) {
    if (right) {
        if (selection_complete_) {
            show_quick_menu(source, point);
        } else {
            finish({ExitCode::user_cancelled, L"已取消。"});
        }
        return;
    }
    if (selection_complete_) {
        for (const auto& button : toolbar_) {
            if (button.id == L"|") continue;
            if (button.bounds.contains(point)) {
                invoke(button.id, source);
                return;
            }
        }
        for (const auto& button : sub_toolbar_) {
            if (button.id == L"|") continue;
            if (button.bounds.contains(point)) {
                invoke_sub(button.id, source);
                invalidate_all();
                return;
            }
        }

        DragMode mode = hit_test_drag_mode(point);
        if (mode == DragMode::none) {
            return;
        }

        if (mode == DragMode::annotate) {
            POINT relative{point.x - selection_.left, point.y - selection_.top};
            if (active_tool_ == Tool::select) {
                selected_annotation_idx_ = -1;
                for (int i = static_cast<int>(annotations_.size()) - 1; i >= 0; --i) {
                    if (hit_annotation(annotations_[i], relative)) {
                        selected_annotation_idx_ = i;
                        original_annotation_ = annotations_[i];
                        break;
                    }
                }
                if (selected_annotation_idx_ != -1) {
                    dragging_selection_ = true;
                    current_drag_mode_ = DragMode::annotate;
                    drag_start_ = point;
                    SetCapture(source);
                    invalidate_all();
                } else {
                    selected_annotation_idx_ = -1;
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
                if (auto text = prompt_text(source, point)) {
                    discard_redo();
                    annotations_.push_back({Tool::text, relative, relative, {}, std::move(*text), active_color_, active_text_size_});
                    finish_annotation();
                }
                return;
            }
            if (active_tool_ == Tool::serial) {
                discard_redo();
                annotations_.push_back({Tool::serial,
                                        relative,
                                        relative,
                                        {},
                                        {},
                                        active_color_,
                                        active_width_,
                                        255,
                                        std::max(1, request_.config.annotation_next_serial)});
                request_.config.annotation_next_serial = std::max(1, request_.config.annotation_next_serial) + 1;
                finish_annotation();
                return;
            }
            if (active_tool_ == Tool::eraser) {
                erase_annotation_at(relative);
                drawing_annotation_ = true;
                preview_ = {active_tool_, relative, relative, {relative}, {}, active_color_, active_width_};
                current_drag_mode_ = DragMode::annotate;
                SetCapture(source);
                return;
            }
            drawing_annotation_ = true;
            preview_ = {active_tool_,
                        relative,
                        relative,
                        {},
                        {},
                        active_color_,
                        active_width_,
                        active_tool_ == Tool::highlight ? active_highlight_alpha_ : 255};
            if (tool_uses_points(active_tool_)) {
                preview_.points.push_back(relative);
            }
            current_drag_mode_ = DragMode::annotate;
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
    if (selection_complete_ && drawing_annotation_) {
        const POINT relative{point.x - selection_.left, point.y - selection_.top};
        preview_.end = relative;
        if (preview_.tool == Tool::eraser) {
            if (selection_.contains(point)) {
                erase_annotation_at(relative);
            }
            return;
        }
        if (tool_uses_points(preview_.tool) && selection_.contains(point)) {
            if (preview_.points.empty() || std::abs(relative.x - preview_.points.back().x) > 2 ||
                std::abs(relative.y - preview_.points.back().y) > 2) {
                preview_.points.push_back(relative);
            }
        }
        invalidate_all();
        return;
    }
    if (dragging_selection_) {
        if (current_drag_mode_ == DragMode::none) {
            int x1 = drag_start_.x;
            int y1 = drag_start_.y;
            int x2 = snap_coordinate(point.x, true);
            int y2 = snap_coordinate(point.y, false);
            selection_ = RectI{x1, y1, x2, y2}.normalized();
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
            } else if (current_drag_mode_ == DragMode::annotate && active_tool_ == Tool::select && selected_annotation_idx_ != -1) {
                auto& selected = annotations_[selected_annotation_idx_];
                selected.start.x = original_annotation_.start.x + dx;
                selected.start.y = original_annotation_.start.y + dy;
                selected.end.x = original_annotation_.end.x + dx;
                selected.end.y = original_annotation_.end.y + dy;
                if (!original_annotation_.points.empty()) {
                    selected.points.resize(original_annotation_.points.size());
                    for (std::size_t i = 0; i < original_annotation_.points.size(); ++i) {
                        selected.points[i].x = original_annotation_.points[i].x + dx;
                        selected.points[i].y = original_annotation_.points[i].y + dy;
                    }
                }
            } else {
                int left = selection_.left;
                int top = selection_.top;
                int right = selection_.right;
                int bottom = selection_.bottom;

                switch (current_drag_mode_) {
                    case DragMode::top_left:
                        left = snap_coordinate(original_selection_.left + dx, true);
                        top = snap_coordinate(original_selection_.top + dy, false);
                        break;
                    case DragMode::top:
                        top = snap_coordinate(original_selection_.top + dy, false);
                        break;
                    case DragMode::top_right:
                        right = snap_coordinate(original_selection_.right + dx, true);
                        top = snap_coordinate(original_selection_.top + dy, false);
                        break;
                    case DragMode::right:
                        right = snap_coordinate(original_selection_.right + dx, true);
                        break;
                    case DragMode::bottom_right:
                        right = snap_coordinate(original_selection_.right + dx, true);
                        bottom = snap_coordinate(original_selection_.bottom + dy, false);
                        break;
                    case DragMode::bottom:
                        bottom = snap_coordinate(original_selection_.bottom + dy, false);
                        break;
                    case DragMode::bottom_left:
                        left = snap_coordinate(original_selection_.left + dx, true);
                        bottom = snap_coordinate(original_selection_.bottom + dy, false);
                        break;
                    case DragMode::left:
                        left = snap_coordinate(original_selection_.left + dx, true);
                        break;
                    default:
                        break;
                }

                selection_ = {left, top, right, bottom};
            }
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
    RectI next{};
    for (const auto& candidate : window_candidates_) {
        if (candidate.bounds.contains(point)) {
            next = candidate.bounds;
            break;
        }
    }
    hover_ = next;
    invalidate_all();
}

void OverlaySession::on_mouse_up(POINT point) {
    ReleaseCapture();
    if (selection_complete_ && drawing_annotation_) {
        drawing_annotation_ = false;
        if (preview_.tool != Tool::eraser && annotation_has_size(preview_)) {
            discard_redo();
            annotations_.push_back(preview_);
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
        current_drag_mode_ = DragMode::none;
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

void OverlaySession::on_double_click(POINT point) {
    if (selection_complete_ && selection_.contains(point) && !is_over_toolbar(point)) {
        complete_clipboard();
    }
}

void OverlaySession::on_key_down(HWND source, WPARAM key) {
    if (key == VK_ESCAPE) {
        finish({ExitCode::user_cancelled, L"已取消。"});
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
            (void)copy_text_to_clipboard(color_str);

            custom_color_ = color;
            request_.config.custom_color = format_hex_color(color);

            finish({ExitCode::success, std::format(L"已复制颜色 {} 到剪贴板。", color_str)});
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

    // After selection is complete: normal editing shortcuts
    auto is_shortcut_triggered = [this](std::wstring_view shortcut_str, WPARAM k) {
        auto hotkey = parse_hotkey(shortcut_str);
        if (!hotkey) return false;
        if (hotkey->virtual_key != k) return false;
        
        bool ctrl = (hotkey->modifiers & MOD_CONTROL) != 0;
        bool alt = (hotkey->modifiers & MOD_ALT) != 0;
        bool shift = (hotkey->modifiers & MOD_SHIFT) != 0;
        bool win = (hotkey->modifiers & MOD_WIN) != 0;
        
        bool actual_ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool actual_alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        bool actual_shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool actual_win = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
        
        return ctrl == actual_ctrl && alt == actual_alt && shift == actual_shift && win == actual_win;
    };

    Tool target_tool = Tool::none;
    if (is_shortcut_triggered(request_.config.tool_shortcut_select, key)) target_tool = Tool::select;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_rectangle, key)) target_tool = Tool::rectangle;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_ellipse, key)) target_tool = Tool::ellipse;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_line, key)) target_tool = Tool::line;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_arrow, key)) target_tool = Tool::arrow;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_pen, key)) target_tool = Tool::pen;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_mosaic, key)) target_tool = Tool::mosaic;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_blur, key)) target_tool = Tool::blur;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_highlight, key)) target_tool = Tool::highlight;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_text, key)) target_tool = Tool::text;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_serial, key)) target_tool = Tool::serial;
    else if (is_shortcut_triggered(request_.config.tool_shortcut_eraser, key)) target_tool = Tool::eraser;

    if (target_tool != Tool::none) {
        active_tool_ = target_tool;
        selected_annotation_idx_ = -1;
        build_toolbar();
        build_sub_toolbar();
        invalidate_all();
        return;
    }

    // Arrow keys for micro-adjusting selection
    if (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT) {
        int step = (GetKeyState(VK_CONTROL) & 0x8000) ? 10 : 1;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift) {
            if (key == VK_LEFT) {
                selection_.right = std::max(selection_.left + 2, selection_.right - step);
            } else if (key == VK_RIGHT) {
                selection_.right = std::min(virtual_bounds_.right, selection_.right + step);
            } else if (key == VK_UP) {
                selection_.bottom = std::max(selection_.top + 2, selection_.bottom - step);
            } else if (key == VK_DOWN) {
                selection_.bottom = std::min(virtual_bounds_.bottom, selection_.bottom + step);
            }
        } else {
            int dx = 0;
            int dy = 0;
            if (key == VK_LEFT) dx = -step;
            else if (key == VK_RIGHT) dx = step;
            else if (key == VK_UP) dy = -step;
            else if (key == VK_DOWN) dy = step;

            int w = selection_.width();
            int h = selection_.height();
            int left = selection_.left + dx;
            int top = selection_.top + dy;

            if (left < virtual_bounds_.left) left = virtual_bounds_.left;
            if (left + w > virtual_bounds_.right) left = virtual_bounds_.right - w;
            if (top < virtual_bounds_.top) top = virtual_bounds_.top;
            if (top + h > virtual_bounds_.bottom) top = virtual_bounds_.bottom - h;

            selection_ = {left, top, left + w, top + h};
        }
        build_toolbar();
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

    // Delete/Backspace key to delete selected annotation
    if (active_tool_ == Tool::select && selected_annotation_idx_ != -1) {
        if (key == VK_DELETE || key == VK_BACK) {
            discard_redo();
            annotations_.erase(annotations_.begin() + selected_annotation_idx_);
            selected_annotation_idx_ = -1;
            build_toolbar();
            invalidate_all();
            return;
        }
    }

    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && key == 'C') {
        complete_clipboard();
        return;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && key == L'Z') {
        undo();
        return;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && key == L'Y') {
        redo();
        return;
    }
    if (request_.config.ocr_enabled && (GetKeyState(VK_SHIFT) & 0x8000) != 0 && key == L'C') {
        complete_ocr();
        return;
    }
    if (key == VK_RETURN) {
        complete_clipboard();
        return;
    }
    if (key == L'S' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        complete_file({}, source);
    }
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

[[nodiscard]] bool OverlaySession::is_over_toolbar(POINT point) const noexcept {
    for (const auto& button : toolbar_) {
        if (button.bounds.contains(point)) return true;
    }
    for (const auto& button : sub_toolbar_) {
        if (button.bounds.contains(point)) return true;
    }
    return false;
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
    if (active_tool_ == Tool::text) {
        float step = delta > 0 ? 2.0F : -2.0F;
        active_text_size_ = std::clamp(active_text_size_ + step, 12.0F, 72.0F);
        if (selected_annotation_idx_ != -1 && static_cast<std::size_t>(selected_annotation_idx_) < annotations_.size()) {
            if (annotations_[selected_annotation_idx_].tool == Tool::text) {
                annotations_[selected_annotation_idx_].width = active_text_size_;
            }
        }
        invalidate_all();
    } else if (tool_supports_width(active_tool_)) {
        float step = delta > 0 ? 1.0F : -1.0F;
        active_width_ = std::clamp(active_width_ + step, 1.0F, 50.0F);
        if (selected_annotation_idx_ != -1 && static_cast<std::size_t>(selected_annotation_idx_) < annotations_.size()) {
            if (tool_supports_width(annotations_[selected_annotation_idx_].tool)) {
                annotations_[selected_annotation_idx_].width = active_width_;
            }
        }
        invalidate_all();
    }
}

void OverlaySession::show_quick_menu(HWND hwnd, POINT pt) {
    HMENU menu = CreatePopupMenu();
    
    // Tools Submenu
    HMENU tools_menu = CreatePopupMenu();
    AppendMenuW(tools_menu, MF_STRING, 101, L"选择工具 (Select)\tSelect");
    AppendMenuW(tools_menu, MF_STRING, 102, L"矩形 (Rectangle)\tRectangle");
    AppendMenuW(tools_menu, MF_STRING, 103, L"椭圆 (Ellipse)\tEllipse");
    AppendMenuW(tools_menu, MF_STRING, 104, L"直线 (Line)\tLine");
    AppendMenuW(tools_menu, MF_STRING, 105, L"箭头 (Arrow)\tArrow");
    AppendMenuW(tools_menu, MF_STRING, 106, L"画笔 (Pen)\tPen");
    AppendMenuW(tools_menu, MF_STRING, 107, L"马赛克 (Mosaic)\tMosaic");
    AppendMenuW(tools_menu, MF_STRING, 108, L"模糊 (Blur)\tBlur");
    AppendMenuW(tools_menu, MF_STRING, 109, L"高亮 (Highlight)\tHighlight");
    AppendMenuW(tools_menu, MF_STRING, 110, L"文本 (Text)\tText");
    AppendMenuW(tools_menu, MF_STRING, 111, L"序号 (Serial)\tSerial");
    AppendMenuW(tools_menu, MF_STRING, 112, L"橡皮擦 (Eraser)\tEraser");
    
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tools_menu), L"工具 (Tools)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    
    AppendMenuW(menu, MF_STRING, 1, L"复制到剪贴板 (Copy)\tCtrl+C / Enter");
    AppendMenuW(menu, MF_STRING, 2, L"保存到文件 (Save)\tCtrl+S");
    AppendMenuW(menu, MF_STRING, 3, L"贴图 (Pin)");
    if (request_.config.ocr_enabled) {
        AppendMenuW(menu, MF_STRING, 4, L"屏幕识字 (OCR)\tShift+C");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    
    AppendMenuW(menu, MF_STRING, 5, L"撤销 (Undo)\tCtrl+Z");
    AppendMenuW(menu, MF_STRING, 6, L"重做 (Redo)\tCtrl+Y");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 7, L"退出 (Exit)\tEsc");

    // Show menu
    int selection = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(tools_menu);
    DestroyMenu(menu);

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
    } else if (selection >= 101 && selection <= 112) {
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
        }
        active_tool_ = chosen_tool;
        selected_annotation_idx_ = -1;
        build_toolbar();
        build_sub_toolbar();
        invalidate_all();
    }
}

}  // namespace airshot::overlay_detail
