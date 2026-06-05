    explicit OverlaySession(RegionRequest request) : request_(std::move(request)) {
        custom_color_ = parse_hex_color(request_.config.custom_color, RGB(128, 0, 255));
    }

    RegionResult run() {
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

    DragMode hit_test_drag_mode(POINT point) const {
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

    void on_mouse_down(HWND source, POINT point, bool right) {
        if (right) {
            finish({ExitCode::user_cancelled, L"已取消。"});
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
                if (active_tool_ == Tool::text) {
                    if (auto text = prompt_text(source, point)) {
                        discard_redo();
                        annotations_.push_back({Tool::text, relative, relative, {}, std::move(*text), active_color_, active_width_});
                        invalidate_all();
                    }
                    return;
                }
                drawing_annotation_ = true;
                preview_ = {active_tool_, relative, relative, {}, {}, active_color_, active_width_};
                if (active_tool_ == Tool::mosaic) {
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

    void on_mouse_move(POINT point) {
        cursor_pos_ = point;
        if (selection_complete_ && drawing_annotation_) {
            preview_.end = {point.x - selection_.left, point.y - selection_.top};
            if (preview_.tool == Tool::mosaic && selection_.contains(point)) {
                const POINT relative{point.x - selection_.left, point.y - selection_.top};
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

    void on_mouse_up(POINT point) {
        ReleaseCapture();
        if (selection_complete_ && drawing_annotation_) {
            drawing_annotation_ = false;
            if (preview_.tool == Tool::mosaic ? preview_.points.size() > 1
                                              : (preview_.start.x != preview_.end.x ||
                                                 preview_.start.y != preview_.end.y)) {
                discard_redo();
                annotations_.push_back(preview_);
            }
            preview_ = {};
            current_drag_mode_ = DragMode::none;
            invalidate_all();
            return;
        }
        if (!dragging_selection_) {
            return;
        }
        dragging_selection_ = false;

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

    void on_double_click(POINT point) {
        if (selection_complete_ && selection_.contains(point) && !is_over_toolbar(point)) {
            complete_clipboard();
        }
    }

    void on_key_down(HWND source, WPARAM key) {
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

    void invalidate_all() const {
        for (const auto& window : windows_) {
            window->invalidate();
        }
    }

    [[nodiscard]] RectI display_selection() const {
        if (!selection_.empty()) {
            return selection_;
        }
        return hover_;
    }

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
    [[nodiscard]] bool is_over_toolbar(POINT point) const noexcept {
        for (const auto& button : toolbar_) {
            if (button.bounds.contains(point)) return true;
        }
        for (const auto& button : sub_toolbar_) {
            if (button.bounds.contains(point)) return true;
        }
        return false;
    }
    [[nodiscard]] COLORREF get_pixel_color(int x, int y) const noexcept {
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
    [[nodiscard]] int snap_coordinate(int value, bool is_x, int threshold = 8) const noexcept {
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
