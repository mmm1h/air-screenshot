#include "overlay_session.h"

namespace airshot::overlay_detail {
namespace {

bool hidden(const AppConfig& config, std::wstring_view id) {
    return annotation_tool_hidden(config.annotation_hidden_tools, id);
}

void trim_trailing_separators(std::vector<std::pair<std::wstring, std::wstring>>& items) {
    while (!items.empty() && items.back().first == L"|") {
        items.pop_back();
    }
}

void add_separator(std::vector<std::pair<std::wstring, std::wstring>>& items) {
    if (!items.empty() && items.back().first != L"|") {
        items.push_back({L"|", L""});
    }
}

struct ToolbarMetrics {
    int button_width;
    int button_height;
    int spacing;
    int padding;
};

struct ToolbarRow {
    std::vector<std::pair<std::wstring, std::wstring>> items;
    int width{};
};

int item_width(std::wstring_view id, const ToolbarMetrics& metrics) {
    return id == L"|" ? 12 : metrics.button_width;
}

int row_width(const std::vector<std::pair<std::wstring, std::wstring>>& items, const ToolbarMetrics& metrics) {
    int width = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index > 0) {
            width += metrics.spacing;
        }
        width += item_width(items[index].first, metrics);
    }
    return width;
}

void trim_row(ToolbarRow& row, const ToolbarMetrics& metrics) {
    while (!row.items.empty() && row.items.back().first == L"|") {
        row.items.pop_back();
    }
    row.width = row_width(row.items, metrics);
}

std::vector<ToolbarRow> wrap_toolbar_items(const std::vector<std::pair<std::wstring, std::wstring>>& items,
                                           const ToolbarMetrics& metrics,
                                           const RectI& bounds) {
    const int available_width = std::max(metrics.button_width, bounds.width() - 2 * metrics.padding);
    std::vector<ToolbarRow> rows;
    ToolbarRow current;
    for (const auto& item : items) {
        const bool separator = item.first == L"|";
        if (separator && current.items.empty()) {
            continue;
        }
        const int width = item_width(item.first, metrics);
        const int next_width = current.items.empty() ? width : current.width + metrics.spacing + width;
        if (!current.items.empty() && next_width > available_width) {
            trim_row(current, metrics);
            if (!current.items.empty()) {
                rows.push_back(std::move(current));
            }
            current = {};
            if (separator) {
                continue;
            }
        }
        current.width = current.items.empty() ? width : current.width + metrics.spacing + width;
        current.items.push_back(item);
    }
    trim_row(current, metrics);
    if (!current.items.empty()) {
        rows.push_back(std::move(current));
    }
    return rows;
}

int toolbar_width(const std::vector<ToolbarRow>& rows, const ToolbarMetrics& metrics) {
    int width = 0;
    for (const auto& row : rows) {
        width = std::max(width, row.width);
    }
    return width + 2 * metrics.padding;
}

int toolbar_height(const std::vector<ToolbarRow>& rows, const ToolbarMetrics& metrics) {
    if (rows.empty()) {
        return 0;
    }
    return static_cast<int>(rows.size()) * metrics.button_height +
           static_cast<int>(rows.size() - 1) * metrics.spacing + 2 * metrics.padding;
}

int clamp_axis(int value, int size, int minimum, int maximum) {
    if (size >= maximum - minimum) {
        return minimum;
    }
    return std::clamp(value, minimum, maximum - size);
}

void place_toolbar_rows(std::vector<ToolbarButton>& target,
                        const std::vector<ToolbarRow>& rows,
                        const ToolbarMetrics& metrics,
                        int left,
                        int top) {
    target.clear();
    int y = top + metrics.padding;
    for (const auto& row : rows) {
        int x = left + metrics.padding;
        for (const auto& item : row.items) {
            const int width = item_width(item.first, metrics);
            target.push_back({item.first, item.second, {x, y, x + width, y + metrics.button_height}});
            x += width + metrics.spacing;
        }
        y += metrics.button_height + metrics.spacing;
    }
}

RectI buttons_bounds(const std::vector<ToolbarButton>& buttons) {
    if (buttons.empty()) {
        return {};
    }
    RectI bounds = buttons.front().bounds;
    for (const auto& button : buttons) {
        bounds.left = std::min(bounds.left, button.bounds.left);
        bounds.top = std::min(bounds.top, button.bounds.top);
        bounds.right = std::max(bounds.right, button.bounds.right);
        bounds.bottom = std::max(bounds.bottom, button.bounds.bottom);
    }
    return bounds;
}

Tool tool_from_id(std::wstring_view id) {
    if (id == L"select") return Tool::select;
    if (id == L"rect") return Tool::rectangle;
    if (id == L"ellipse") return Tool::ellipse;
    if (id == L"line") return Tool::line;
    if (id == L"arrow") return Tool::arrow;
    if (id == L"pen") return Tool::pen;
    if (id == L"mosaic") return Tool::mosaic;
    if (id == L"blur") return Tool::blur;
    if (id == L"highlight") return Tool::highlight;
    if (id == L"text") return Tool::text;
    if (id == L"serial") return Tool::serial;
    if (id == L"eraser") return Tool::eraser;
    return Tool::none;
}

bool tool_supports_color(Tool tool) {
    return tool == Tool::rectangle || tool == Tool::ellipse || tool == Tool::line || tool == Tool::arrow ||
           tool == Tool::pen || tool == Tool::highlight || tool == Tool::text || tool == Tool::serial;
}

bool tool_supports_width(Tool tool) {
    return tool == Tool::rectangle || tool == Tool::ellipse || tool == Tool::line || tool == Tool::arrow ||
           tool == Tool::pen || tool == Tool::mosaic || tool == Tool::highlight || tool == Tool::eraser ||
           tool == Tool::blur;
}

bool tool_supports_alpha(Tool tool) {
    return tool == Tool::highlight;
}

void blend_pixel(Bitmap& bitmap, int x, int y, COLORREF color, int alpha) {
    if (x < 0 || y < 0 || x >= bitmap.width || y >= bitmap.height) {
        return;
    }
    alpha = std::clamp(alpha, 0, 255);
    const std::size_t index = static_cast<std::size_t>(y * bitmap.width + x) * 4U;
    auto blend = [alpha](std::uint8_t dst, std::uint8_t src) {
        return static_cast<std::uint8_t>((static_cast<int>(src) * alpha + static_cast<int>(dst) * (255 - alpha)) / 255);
    };
    bitmap.pixels[index] = blend(bitmap.pixels[index], GetBValue(color));
    bitmap.pixels[index + 1] = blend(bitmap.pixels[index + 1], GetGValue(color));
    bitmap.pixels[index + 2] = blend(bitmap.pixels[index + 2], GetRValue(color));
    bitmap.pixels[index + 3] = 255;
}

void blend_circle(Bitmap& bitmap, POINT center, int radius, COLORREF color, int alpha) {
    const int radius_sq = radius * radius;
    for (int y = center.y - radius; y <= center.y + radius; ++y) {
        for (int x = center.x - radius; x <= center.x + radius; ++x) {
            const int dx = x - center.x;
            const int dy = y - center.y;
            if (dx * dx + dy * dy <= radius_sq) {
                blend_pixel(bitmap, x, y, color, alpha);
            }
        }
    }
}

void blend_line(Bitmap& bitmap, POINT start, POINT end, int radius, COLORREF color, int alpha) {
    const int dx = end.x - start.x;
    const int dy = end.y - start.y;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps <= 0) {
        blend_circle(bitmap, start, radius, color, alpha);
        return;
    }
    for (int index = 0; index <= steps; ++index) {
        const double t = static_cast<double>(index) / static_cast<double>(steps);
        POINT point{
            static_cast<LONG>(std::lround(start.x + dx * t)),
            static_cast<LONG>(std::lround(start.y + dy * t)),
        };
        blend_circle(bitmap, point, radius, color, alpha);
    }
}

void draw_polyline(HDC dc, const std::vector<POINT>& points) {
    if (points.empty()) {
        return;
    }
    MoveToEx(dc, points.front().x, points.front().y, nullptr);
    for (std::size_t index = 1; index < points.size(); ++index) {
        LineTo(dc, points[index].x, points[index].y);
    }
}

}  // namespace

void OverlaySession::build_toolbar() {
    toolbar_.clear();
    std::vector<std::pair<std::wstring, std::wstring>> items;

    auto split_by_comma = [](std::wstring_view value) {
        std::vector<std::wstring> result;
        std::size_t start = 0;
        while (start <= value.size()) {
            const std::size_t end = value.find(L',', start);
            result.emplace_back(value.substr(start, end == std::wstring_view::npos ? value.size() - start : end - start));
            if (end == std::wstring_view::npos) {
                break;
            }
            start = end + 1;
        }
        return result;
    };

    auto get_tool_label = [&](std::wstring_view id) -> std::wstring {
        if (id == L"lock") return std::wstring(strings::toolbar_lock);
        if (id == L"select") return L"选";
        if (id == L"rect") return std::wstring(strings::toolbar_rectangle);
        if (id == L"ellipse") return std::wstring(strings::toolbar_ellipse);
        if (id == L"line") return std::wstring(strings::toolbar_line);
        if (id == L"arrow") return std::wstring(strings::toolbar_arrow);
        if (id == L"pen") return std::wstring(strings::toolbar_pen);
        if (id == L"mosaic") return std::wstring(strings::toolbar_mosaic);
        if (id == L"blur") return L"糊";
        if (id == L"highlight") return std::wstring(strings::toolbar_highlight);
        if (id == L"text") return std::wstring(strings::toolbar_text);
        if (id == L"serial") return std::wstring(strings::toolbar_serial);
        if (id == L"eraser") return std::wstring(strings::toolbar_eraser);
        if (id == L"undo") return std::wstring(strings::toolbar_undo);
        if (id == L"redo") return std::wstring(strings::toolbar_redo);
        if (id == L"ocr") return std::wstring(strings::toolbar_ocr);
        if (id == L"scroll") return L"长";
        if (id == L"pin") return L"钉";
        if (id == L"copy") return std::wstring(strings::toolbar_copy);
        if (id == L"save") return std::wstring(strings::toolbar_save);
        if (id == L"close") return std::wstring(strings::toolbar_close);
        return L"";
    };

    auto get_tool_category = [](std::wstring_view id) -> int {
        if (id == L"lock") return 1;
        if (id == L"select" || id == L"rect" || id == L"ellipse" || id == L"line" ||
            id == L"arrow" || id == L"pen" || id == L"mosaic" || id == L"blur" ||
            id == L"highlight" || id == L"text" || id == L"serial" || id == L"eraser") {
            return 2;
        }
        if (id == L"undo" || id == L"redo") return 3;
        if (id == L"ocr" || id == L"scroll" || id == L"pin") return 4;
        if (id == L"copy" || id == L"save" || id == L"close") return 5;
        return 0;
    };

    int last_category = -1;
    for (const auto& token : split_by_comma(request_.config.toolbar_order)) {
        if (token == L"undo" && annotations_.empty()) continue;
        if (token == L"redo" && redo_.empty()) continue;
        if (token == L"ocr" && !request_.config.ocr_enabled) continue;
        if (token == L"lock" && !request_.config.annotation_enabled) continue;
        
        int cat = get_tool_category(token);
        if (cat == 2 && !request_.config.annotation_enabled) continue;
        if (cat == 3 && !request_.config.annotation_enabled) continue;
        
        if (hidden(request_.config, token)) continue;
        
        std::wstring label = get_tool_label(token);
        if (label.empty()) continue;
        
        if (last_category != -1 && last_category != cat) {
            add_separator(items);
        }
        items.push_back({std::wstring(token), label});
        last_category = cat;
    }
    trim_trailing_separators(items);

    const ToolbarMetrics metrics{36, 32, 4, 6};
    const auto rows = wrap_toolbar_items(items, metrics, virtual_bounds_);
    const int total_width = toolbar_width(rows, metrics);
    const int total_height = toolbar_height(rows, metrics);
    if (rows.empty()) {
        toolbar_.clear();
        sub_toolbar_.clear();
        return;
    }

    const int preferred_left = selection_.right - total_width;
    const int left = clamp_axis(preferred_left, total_width, virtual_bounds_.left, virtual_bounds_.right);
    const int below_top = selection_.bottom + 6;
    const int above_top = selection_.top - total_height - 6;
    int top = below_top;
    if (below_top + total_height > virtual_bounds_.bottom && above_top >= virtual_bounds_.top) {
        top = above_top;
    }
    top = clamp_axis(top, total_height, virtual_bounds_.top, virtual_bounds_.bottom);

    place_toolbar_rows(toolbar_, rows, metrics, left, top);
    build_sub_toolbar();
}

void OverlaySession::build_sub_toolbar() {
    sub_toolbar_.clear();
    if (active_tool_ == Tool::none) {
        return;
    }
    std::vector<std::pair<std::wstring, std::wstring>> items;
    if (tool_supports_color(active_tool_)) {
        items.push_back({L"color_red", L"红"});
        items.push_back({L"color_green", L"绿"});
        items.push_back({L"color_blue", L"蓝"});
        items.push_back({L"color_yellow", L"黄"});
        items.push_back({L"color_black", L"黑"});
        items.push_back({L"color_white", L"白"});
        items.push_back({L"color_custom", L"自"});
    }
    if (tool_supports_width(active_tool_)) {
        if (!items.empty()) {
            items.push_back({L"|", L""});
        }
        items.push_back({L"width_small", L"细"});
        items.push_back({L"width_medium", L"中"});
        items.push_back({L"width_large", L"粗"});
    }
    if (tool_supports_alpha(active_tool_)) {
        if (!items.empty()) {
            items.push_back({L"|", L""});
        }
        items.push_back({L"alpha_low", L"浅"});
        items.push_back({L"alpha_medium", L"中"});
        items.push_back({L"alpha_high", L"深"});
    }
    if (items.empty()) {
        return;
    }

    if (toolbar_.empty()) return;
    const ToolbarMetrics metrics{32, 30, 4, 6};
    const auto rows = wrap_toolbar_items(items, metrics, virtual_bounds_);
    const int total_width = toolbar_width(rows, metrics);
    const int total_height = toolbar_height(rows, metrics);
    if (rows.empty()) {
        return;
    }

    const RectI anchor = buttons_bounds(toolbar_);
    const int left = clamp_axis(anchor.left - metrics.padding, total_width, virtual_bounds_.left, virtual_bounds_.right);
    const bool main_above = anchor.top < selection_.top;
    const int preferred_top = main_above ? anchor.top - total_height - 6 : anchor.bottom + 6;
    const int fallback_top = main_above ? anchor.bottom + 6 : anchor.top - total_height - 6;
    int top = preferred_top;
    if ((top < virtual_bounds_.top || top + total_height > virtual_bounds_.bottom) &&
        fallback_top >= virtual_bounds_.top && fallback_top + total_height <= virtual_bounds_.bottom) {
        top = fallback_top;
    }
    top = clamp_axis(top, total_height, virtual_bounds_.top, virtual_bounds_.bottom);

    place_toolbar_rows(sub_toolbar_, rows, metrics, left, top);
}

void OverlaySession::invoke_sub(std::wstring_view id, HWND source) {
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
    } else if (id == L"alpha_low") {
        active_highlight_alpha_ = 64;
        request_.config.annotation_highlight_alpha = active_highlight_alpha_;
    } else if (id == L"alpha_medium") {
        active_highlight_alpha_ = 96;
        request_.config.annotation_highlight_alpha = active_highlight_alpha_;
    } else if (id == L"alpha_high") {
        active_highlight_alpha_ = 144;
        request_.config.annotation_highlight_alpha = active_highlight_alpha_;
    }
}

void OverlaySession::invoke(std::wstring_view id, HWND source) {
    if (id == L"lock") {
        request_.config.annotation_locked_tool = !request_.config.annotation_locked_tool;
    } else if (const Tool tool = tool_from_id(id); tool != Tool::none) {
        const bool switching_on = active_tool_ != tool;
        active_tool_ = switching_on ? tool : Tool::none;
        if (switching_on && tool == Tool::highlight && active_color_ == RGB(245, 34, 45)) {
            active_color_ = RGB(250, 219, 20);
        }
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
    build_toolbar();
    build_sub_toolbar();
    invalidate_all();
}

void OverlaySession::finish_annotation() {
    if (!request_.config.annotation_locked_tool) {
        active_tool_ = Tool::none;
    }
    build_toolbar();
    invalidate_all();
}

void OverlaySession::undo() {
    if (!annotations_.empty()) {
        redo_.push_back(std::move(annotations_.back()));
        annotations_.pop_back();
        invalidate_all();
    }
}

void OverlaySession::redo() {
    if (!redo_.empty()) {
        annotations_.push_back(std::move(redo_.back()));
        redo_.pop_back();
        invalidate_all();
    }
}

void OverlaySession::discard_redo() {
    redo_.clear();
}

Bitmap OverlaySession::original_selection() const {
    return compose_selection(monitors_, selection_);
}

Bitmap OverlaySession::rendered_selection() const {
    Bitmap result = original_selection();
    for (const auto& annotation : annotations_) {
        if (annotation.tool == Tool::mosaic) {
            for (const POINT point : annotation.points) {
                pixelate_circle(result, point, 14, 8);
            }
        } else if (annotation.tool == Tool::blur) {
            for (const POINT point : annotation.points) {
                int radius = static_cast<int>(annotation.width * 3.5F);
                if (radius < 5) radius = 5;
                blur_circle(result, point, radius, 6);
            }
        } else if (annotation.tool == Tool::highlight) {
            const int radius = std::max(4, static_cast<int>(std::lround(annotation.width * 1.6F)));
            if (annotation.points.size() > 1) {
                for (std::size_t index = 1; index < annotation.points.size(); ++index) {
                    blend_line(result, annotation.points[index - 1], annotation.points[index], radius, annotation.color, annotation.alpha);
                }
            } else {
                blend_line(result, annotation.start, annotation.end, radius, annotation.color, annotation.alpha);
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
            const RectI bounds{annotation.start.x, annotation.start.y, annotation.end.x, annotation.end.y};
            const RectI normalized = bounds.normalized();
            Rectangle(dc, normalized.left, normalized.top, normalized.right, normalized.bottom);
        } else if (annotation.tool == Tool::ellipse) {
            const RectI bounds{annotation.start.x, annotation.start.y, annotation.end.x, annotation.end.y};
            const RectI normalized = bounds.normalized();
            Ellipse(dc, normalized.left, normalized.top, normalized.right, normalized.bottom);
        } else if (annotation.tool == Tool::line) {
            MoveToEx(dc, annotation.start.x, annotation.start.y, nullptr);
            LineTo(dc, annotation.end.x, annotation.end.y);
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
        } else if (annotation.tool == Tool::pen) {
            draw_polyline(dc, annotation.points);
        } else if (annotation.tool == Tool::text) {
            HFONT text_font = CreateFontW(static_cast<int>(annotation.width),
                                         0,
                                         0,
                                         0,
                                         request_.config.text_font_bold ? FW_BOLD : FW_NORMAL,
                                         request_.config.text_font_italic ? TRUE : FALSE,
                                         FALSE,
                                         FALSE,
                                         DEFAULT_CHARSET,
                                         OUT_DEFAULT_PRECIS,
                                         CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY,
                                         DEFAULT_PITCH,
                                         request_.config.text_font_family.c_str());
            HGDIOBJ previous_text_font = SelectObject(dc, text_font);
            TextOutW(dc,
                     annotation.start.x,
                     annotation.start.y,
                     annotation.text.c_str(),
                     static_cast<int>(annotation.text.size()));
            SelectObject(dc, previous_text_font);
            DeleteObject(text_font);
        } else if (annotation.tool == Tool::serial) {
            int radius = static_cast<int>(std::round(8.0F + annotation.width * 1.5F));
            HBRUSH fill_brush = CreateSolidBrush(annotation.color);
            HGDIOBJ old_fill = SelectObject(dc, fill_brush);
            HPEN white_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            HGDIOBJ old_pen_in_bubble = SelectObject(dc, white_pen);

            Ellipse(dc,
                    annotation.start.x - radius,
                    annotation.start.y - radius,
                    annotation.start.x + radius,
                    annotation.start.y + radius);

            SelectObject(dc, old_fill);
            DeleteObject(fill_brush);
            SelectObject(dc, old_pen_in_bubble);
            DeleteObject(white_pen);

            const std::wstring serial_text = std::to_wstring(annotation.serial);
            RECT text_rect{
                annotation.start.x - radius,
                annotation.start.y - radius,
                annotation.start.x + radius,
                annotation.start.y + radius,
            };
            int font_height = static_cast<int>(std::round(radius * 1.0F));
            HFONT serial_font = CreateFontW(font_height,
                                          0, 0, 0,
                                          FW_BOLD,
                                          FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS,
                                          CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY,
                                          DEFAULT_PITCH,
                                          L"Consolas");
            HGDIOBJ old_font_in_bubble = SelectObject(dc, serial_font);
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, serial_text.c_str(), static_cast<int>(serial_text.size()), &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old_font_in_bubble);
            DeleteObject(serial_font);
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

void OverlaySession::complete_clipboard() {
    std::wstring error;
    if (!copy_bitmap_to_clipboard(rendered_selection(), &error)) {
        finish({ExitCode::operation_failed, std::move(error)});
        return;
    }
    finish({ExitCode::success, L"截图已复制到剪贴板。"});
}

void OverlaySession::complete_file(std::wstring_view requested_path, HWND owner) {
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

void OverlaySession::complete_ocr() {
    if (!request_.config.ocr_enabled) {
        finish({ExitCode::module_unavailable, L"OCR 模块已关闭。"});
        return;
    }
    const OcrOutput output = recognize_text(original_selection(), request_.config);
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

void OverlaySession::complete_pin() {
    RegionResult result{ExitCode::success, L"贴图已创建。"};
    result.action = RegionAction::pin;
    result.bitmap = rendered_selection();
    finish(std::move(result));
}

void OverlaySession::complete_scroll(HWND source) {
    for (const auto& window : windows_) {
        ShowWindow(window->hwnd(), SW_HIDE);
    }
    run_scroll_capture(source);
}

void OverlaySession::run_scroll_capture(HWND source) {
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

void OverlaySession::finish(RegionResult result) {
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

}  // namespace airshot::overlay_detail
