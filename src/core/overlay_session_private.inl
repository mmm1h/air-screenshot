    void build_toolbar() {
        toolbar_.clear();
        std::vector<std::pair<std::wstring, std::wstring>> items;
        if (request_.config.annotation_enabled) {
            items.push_back({L"rect", std::wstring(strings::toolbar_rectangle)});
            items.push_back({L"arrow", std::wstring(strings::toolbar_arrow)});
            items.push_back({L"mosaic", std::wstring(strings::toolbar_mosaic)});
            items.push_back({L"text", std::wstring(strings::toolbar_text)});
            bool show_undo = !annotations_.empty();
            bool show_redo = !redo_.empty();
            if (show_undo || show_redo) {
                items.push_back({L"|", L""});
                if (show_undo) {
                    items.push_back({L"undo", std::wstring(strings::toolbar_undo)});
                }
                if (show_redo) {
                    items.push_back({L"redo", std::wstring(strings::toolbar_redo)});
                }
            }
            items.push_back({L"|", L""});
        }
        if (request_.config.ocr_enabled) {
            items.push_back({L"ocr", std::wstring(strings::toolbar_ocr)});
        }
        items.push_back({L"scroll", L"长"});
        items.push_back({L"pin", L"钉"});
        items.push_back({L"|", L""});
        items.push_back({L"copy", std::wstring(strings::toolbar_copy)});
        items.push_back({L"save", std::wstring(strings::toolbar_save)});
        items.push_back({L"close", std::wstring(strings::toolbar_close)});

        constexpr int button_width = 36;
        constexpr int button_height = 32;
        constexpr int spacing = 4;
        constexpr int padding = 6;
        constexpr int toolbar_height = button_height + 2 * padding;

        int total_width = 2 * padding;
        for (std::size_t index = 0; index < items.size(); ++index) {
            int w = (items[index].first == L"|") ? 12 : button_width;
            total_width += w;
            if (index > 0) {
                total_width += spacing;
            }
        }

        int left = std::min(selection_.right - total_width, virtual_bounds_.right - total_width);
        left = std::max(left, virtual_bounds_.left);
        int top = selection_.bottom + 6;
        if (top + toolbar_height > virtual_bounds_.bottom) {
            top = selection_.top - toolbar_height - 6;
        }
        top = std::max(virtual_bounds_.top, top);

        int current_x = left + padding;
        for (std::size_t index = 0; index < items.size(); ++index) {
            int w = (items[index].first == L"|") ? 12 : button_width;
            int y = top + padding;
            toolbar_.push_back({items[index].first, items[index].second, {current_x, y, current_x + w, y + button_height}});
            current_x += w + spacing;
        }
        build_sub_toolbar();
    }

    void build_sub_toolbar() {
        sub_toolbar_.clear();
        if (active_tool_ != Tool::rectangle && active_tool_ != Tool::arrow && active_tool_ != Tool::text) {
            return;
        }
        std::vector<std::pair<std::wstring, std::wstring>> items = {
            {L"color_red", L"红"},
            {L"color_green", L"绿"},
            {L"color_blue", L"蓝"},
            {L"color_yellow", L"黄"},
            {L"color_black", L"黑"},
            {L"color_white", L"白"},
            {L"width_small", L"细"},
            {L"width_medium", L"中"},
            {L"width_large", L"粗"}
        };

        constexpr int button_width = 32;
        constexpr int button_height = 30;
        constexpr int spacing = 4;
        constexpr int padding = 6;
        constexpr int sub_toolbar_height = button_height + 2 * padding;

        if (toolbar_.empty()) return;
        int left = toolbar_.front().bounds.left - padding;

        bool main_above = (toolbar_.front().bounds.top < selection_.top);
        int top = 0;
        if (main_above) {
            top = toolbar_.front().bounds.top - padding - sub_toolbar_height - 6;
        } else {
            top = toolbar_.front().bounds.bottom + padding + 6;
        }

        for (std::size_t i = 0; i < items.size(); ++i) {
            int x = left + padding + static_cast<int>(i) * (button_width + spacing);
            int y = top + padding;
            sub_toolbar_.push_back({items[i].first, items[i].second, {x, y, x + button_width, y + button_height}});
        }
    }

    void invoke_sub(std::wstring_view id, HWND source) {
        if (id == L"color_red") {
            active_color_ = RGB(245, 34, 45);
        } else if (id == L"color_green") {
            active_color_ = RGB(82, 196, 26);
        } else if (id == L"color_blue") {
            active_color_ = RGB(22, 119, 255);
        } else if (id == L"color_yellow") {
            active_color_ = RGB(250, 219, 20);
        } else if (id == L"color_black") {
            active_color_ = RGB(0, 0, 0);
        } else if (id == L"color_white") {
            active_color_ = RGB(255, 255, 255);
        } else if (id == L"color_custom") {
            RectI button_bounds{};
            for (const auto& btn : sub_toolbar_) {
                if (btn.id == L"color_custom") {
                    button_bounds = btn.bounds;
                    break;
                }
            }
            show_rgb_picker_popup(source, custom_color_, button_bounds, virtual_bounds_, [this](COLORREF new_color) {
                custom_color_ = new_color;
                active_color_ = new_color;
                request_.config.custom_color = format_hex_color(new_color);
                invalidate_all();
            });
        } else if (id == L"width_small") {
            active_width_ = 2.0F;
        } else if (id == L"width_medium") {
            active_width_ = 4.0F;
        } else if (id == L"width_large") {
            active_width_ = 8.0F;
        }
    }

    void invoke(std::wstring_view id, HWND source) {
        if (id == L"rect") {
            active_tool_ = Tool::rectangle;
        } else if (id == L"arrow") {
            active_tool_ = Tool::arrow;
        } else if (id == L"mosaic") {
            active_tool_ = Tool::mosaic;
        } else if (id == L"text") {
            active_tool_ = Tool::text;
        } else if (id == L"undo") {
            undo();
        } else if (id == L"redo") {
            redo();
        } else if (id == L"ocr") {
            complete_ocr();
        } else if (id == L"copy") {
            complete_clipboard();
        } else if (id == L"save") {
            complete_file({}, source);
        } else if (id == L"scroll") {
            complete_scroll(source);
        } else if (id == L"pin") {
            complete_pin();
        } else if (id == L"close") {
            finish({ExitCode::user_cancelled, L"已取消。"});
        }
        build_sub_toolbar();
        invalidate_all();
    }

    void undo() {
        if (!annotations_.empty()) {
            redo_.push_back(std::move(annotations_.back()));
            annotations_.pop_back();
            invalidate_all();
        }
    }

    void redo() {
        if (!redo_.empty()) {
            annotations_.push_back(std::move(redo_.back()));
            redo_.pop_back();
            invalidate_all();
        }
    }

    void discard_redo() {
        redo_.clear();
    }

    Bitmap original_selection() const {
        return compose_selection(monitors_, selection_);
    }

    Bitmap rendered_selection() const {
        Bitmap result = original_selection();
        for (const auto& annotation : annotations_) {
            if (annotation.tool == Tool::mosaic) {
                for (const POINT point : annotation.points) {
                    pixelate_circle(result, point, 14, 8);
                }
            }
        }

        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = result.width;
        info.bmiHeader.biHeight = -result.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ previous_bitmap = SelectObject(dc, dib);
        std::memcpy(bits, result.pixels.data(), result.pixels.size());
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        HFONT font = CreateFontW(22,
                                 0,
                                 0,
                                 0,
                                 FW_NORMAL,
                                 FALSE,
                                 FALSE,
                                 FALSE,
                                 DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH,
                                 L"Microsoft YaHei");
        HGDIOBJ previous_font = SelectObject(dc, font);
        for (const auto& annotation : annotations_) {
            HPEN pen = CreatePen(PS_SOLID, static_cast<int>(annotation.width), annotation.color);
            HGDIOBJ previous_pen = SelectObject(dc, pen);
            SetTextColor(dc, annotation.color);

            if (annotation.tool == Tool::rectangle) {
                Rectangle(dc, annotation.start.x, annotation.start.y, annotation.end.x, annotation.end.y);
            } else if (annotation.tool == Tool::arrow) {
                MoveToEx(dc, annotation.start.x, annotation.start.y, nullptr);
                LineTo(dc, annotation.end.x, annotation.end.y);
                const double angle = std::atan2(
                    static_cast<double>(annotation.end.y - annotation.start.y),
                    static_cast<double>(annotation.end.x - annotation.start.x));
                const double length = 10.0 + annotation.width * 2.0;
                for (double offset : {0.45, -0.45}) {
                    MoveToEx(dc, annotation.end.x, annotation.end.y, nullptr);
                    LineTo(dc,
                           annotation.end.x - static_cast<int>(std::cos(angle + offset) * length),
                           annotation.end.y - static_cast<int>(std::sin(angle + offset) * length));
                }
            } else if (annotation.tool == Tool::text) {
                TextOutW(dc,
                         annotation.start.x,
                         annotation.start.y,
                         annotation.text.c_str(),
                         static_cast<int>(annotation.text.size()));
            }
            SelectObject(dc, previous_pen);
            DeleteObject(pen);
        }
        std::memcpy(result.pixels.data(), bits, result.pixels.size());
        SelectObject(dc, previous_font);
        SelectObject(dc, previous_brush);
        SelectObject(dc, previous_bitmap);
        DeleteObject(font);
        DeleteObject(dib);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return result;
    }

    void complete_clipboard() {
        std::wstring error;
        if (!copy_bitmap_to_clipboard(rendered_selection(), &error)) {
            finish({ExitCode::operation_failed, std::move(error)});
            return;
        }
        finish({ExitCode::success, L"截图已复制到剪贴板。"});
    }

    void complete_file(std::wstring_view requested_path, HWND owner) {
        std::optional<std::filesystem::path> path;
        if (requested_path.empty() && request_.action == RegionAction::interactive) {
            path = prompt_png_path(owner);
            if (!path) {
                return;
            }
        } else {
            path = resolve_output_path(requested_path);
        }
        std::wstring error;
        if (!save_png(rendered_selection(), *path, &error)) {
            finish({ExitCode::operation_failed, std::move(error)});
            return;
        }
        RegionResult result{ExitCode::success, L"截图已保存。"};
        result.path = path->wstring();
        finish(std::move(result));
    }

    void complete_ocr() {
        if (!request_.config.ocr_enabled) {
            finish({ExitCode::module_unavailable, L"OCR 模块已关闭。"});
            return;
        }
        const OcrOutput output = recognize_text(original_selection());
        if (!output.ok) {
            finish({ExitCode::operation_failed, output.error});
            return;
        }
        if (request_.copy_ocr) {
            std::wstring error;
            if (!copy_text_to_clipboard(output.text, &error)) {
                finish({ExitCode::operation_failed, std::move(error)});
                return;
            }
        }
        RegionResult result{ExitCode::success, request_.copy_ocr ? std::wstring(strings::ocr_success) : L"OCR 完成。"};
        result.text = output.text;
        finish(std::move(result));
    }

    void complete_pin() {
        RegionResult result{ExitCode::success, L"贴图已创建。"};
        result.action = RegionAction::pin;
        result.bitmap = rendered_selection();
        finish(std::move(result));
    }

    void complete_scroll(HWND source) {
        for (const auto& window : windows_) {
            ShowWindow(window->hwnd(), SW_HIDE);
        }
        run_scroll_capture(source);
    }

    void run_scroll_capture(HWND source) {
        Bitmap stitched = compose_selection(monitors_, selection_);
        if (stitched.empty()) {
            finish({ExitCode::operation_failed, L"长截图初始化失败。"});
            return;
        }
        Bitmap last_stitched_frame = stitched;
        Bitmap last_cap = stitched;

        HINSTANCE instance = GetModuleHandleW(nullptr);
        HWND border_wnd = create_scroll_border_window(instance, source, selection_);
        ScrollControlState control_state;
        HWND control_wnd = create_scroll_control_window(instance, source, selection_, &control_state);
        if (control_wnd) {
            std::wstring progress = std::format(L"{} px", stitched.height);
            SetWindowTextW(control_wnd, progress.c_str());
        }

        UINT_PTR timer_id = SetTimer(source, 999, 80, nullptr);
        int locked_direction = 0; // 0 = undecided, 1 = down, -1 = up
        int consecutive_failures = 0;

        MSG msg{};
        while (IsWindow(control_wnd) && !control_state.finished && !control_state.cancelled) {
            if (GetAsyncKeyState(VK_RETURN) & 0x8000) {
                control_state.finished = true;
                break;
            }
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                control_state.cancelled = true;
                break;
            }
            if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_TIMER && msg.wParam == 999) {
                    Bitmap new_frame = capture_rect(selection_);
                    if (!new_frame.empty()) {
                        ScrollResult res = detect_scroll(last_stitched_frame, new_frame, locked_direction);
                        if (res.matched) {
                            consecutive_failures = 0;
                            if (res.direction != 0 && res.offset > 0) {
                                if (locked_direction == 0) {
                                    locked_direction = res.direction;
                                }
                                if (res.direction == locked_direction) {
                                    if (locked_direction == 1) {
                                        append_to_stitched(stitched, new_frame, res.offset);
                                    } else {
                                        prepend_to_stitched(stitched, new_frame, res.offset);
                                    }
                                    last_stitched_frame = new_frame;
                                }
                            }
                        } else {
                            if (locked_direction != 0) {
                                consecutive_failures++;
                                if (consecutive_failures >= 4) {
                                    // Lost matching anchor. Re-anchor to current frame.
                                    last_stitched_frame = new_frame;
                                    consecutive_failures = 0;
                                }
                            }
                        }

                        std::wstring progress = std::format(L"{} px", stitched.height);
                        SetWindowTextW(control_wnd, progress.c_str());
                        last_cap = new_frame;
                    }
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            } else {
                Sleep(10);
            }
        }

        KillTimer(source, timer_id);
        if (border_wnd) DestroyWindow(border_wnd);
        if (control_wnd) DestroyWindow(control_wnd);

        if (control_state.cancelled) {
            finish({ExitCode::user_cancelled, L"长截图已取消。"});
            return;
        }

        std::wstring error;
        bool saved = false;
        std::wstring path_msg;
        if (request_.action == RegionAction::file || request_.config.default_output == L"file") {
            auto path = prompt_png_path(source);
            if (path) {
                if (save_png(stitched, *path, &error)) {
                    saved = true;
                    path_msg = path->wstring();
                }
            }
        }
        if (!saved) {
            if (!copy_bitmap_to_clipboard(stitched, &error)) {
                finish({ExitCode::operation_failed, std::move(error)});
                return;
            }
        }

        RegionResult result{ExitCode::success, saved ? L"长截图已保存。" : L"长截图已复制到剪贴板。"};
        if (saved) {
            result.path = path_msg;
        }
        result.bitmap = stitched;
        finish(std::move(result));
    }

    void finish(RegionResult result) {
        if (done_) {
            return;
        }
        result_ = std::move(result);
        if (result_.bounds.empty()) {
            result_.bounds = selection_;
        }
        if (result_.action == RegionAction::interactive) {
            result_.action = request_.action;
        }
        result_.config = request_.config;
        done_ = true;
        for (const auto& window : windows_) {
            ShowWindow(window->hwnd(), SW_HIDE);
        }
    }
