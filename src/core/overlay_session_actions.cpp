#include "overlay_session.h"

#include <dwmapi.h>

#include <array>
#include <exception>

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
    if (id == L"drag") return 20;
    if (id == L"|") return 9;
    if (id == L"text_size_btn") return 86;
    if (id == L"mosaic_strength_slider" || id == L"watermark_opacity_slider") return 188;
    if (id == L"watermark_text") return 122;
    if (id == L"watermark_apply" || id == L"watermark_clear") return 54;
    if (id.starts_with(L"effect_") || id.starts_with(L"mode_")) return 68;
    return metrics.button_width;
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

RectI toolbar_host_bounds(const std::vector<MonitorSnapshot>& monitors,
                          const RectI& selection,
                          const RectI& fallback) {
    const MonitorSnapshot* best = nullptr;
    std::int64_t best_area = -1;
    for (const auto& monitor : monitors) {
        const int left = std::max(selection.left, monitor.bounds.left);
        const int top = std::max(selection.top, monitor.bounds.top);
        const int right = std::min(selection.right, monitor.bounds.right);
        const int bottom = std::min(selection.bottom, monitor.bounds.bottom);
        const std::int64_t width = std::max(0, right - left);
        const std::int64_t height = std::max(0, bottom - top);
        const std::int64_t area = width * height;
        if (area > best_area) {
            best = &monitor;
            best_area = area;
        }
    }
    return best ? best->bounds : fallback;
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
    if (id == L"watermark") return Tool::watermark;
    return Tool::none;
}

bool tool_is_feishu_style_shape(Tool tool) {
    return tool == Tool::rectangle || tool == Tool::ellipse || tool == Tool::line || tool == Tool::arrow ||
           tool == Tool::pen;
}

bool tool_supports_color(Tool tool) {
    return tool == Tool::rectangle || tool == Tool::ellipse || tool == Tool::line || tool == Tool::arrow ||
           tool == Tool::pen || tool == Tool::highlight || tool == Tool::text || tool == Tool::serial ||
           tool == Tool::watermark;
}

bool tool_supports_width(Tool tool) {
    return tool == Tool::rectangle || tool == Tool::ellipse || tool == Tool::line || tool == Tool::arrow ||
           tool == Tool::pen || tool == Tool::mosaic || tool == Tool::highlight || tool == Tool::serial ||
           tool == Tool::blur;
}

bool tool_supports_alpha(Tool tool) {
    return tool == Tool::highlight;
}

constexpr std::array<float, 13> kTextSizes{12.0F, 14.0F, 16.0F, 18.0F, 20.0F, 24.0F, 28.0F,
                                           32.0F, 36.0F, 48.0F, 64.0F, 72.0F, 96.0F};

float nearest_text_size(float value) {
    float best = kTextSizes.front();
    float best_distance = std::abs(value - best);
    for (float option : kTextSizes) {
        const float distance = std::abs(value - option);
        if (distance < best_distance) {
            best = option;
            best_distance = distance;
        }
    }
    return best;
}

std::wstring text_size_label(float value) {
    return std::format(L"{:.0f}pt", nearest_text_size(value));
}

void add_feishu_palette(std::vector<std::pair<std::wstring, std::wstring>>& items) {
    items.push_back({L"color_red", L""});
    items.push_back({L"color_yellow", L""});
    items.push_back({L"color_green", L""});
    items.push_back({L"color_blue", L""});
    items.push_back({L"color_black", L""});
    items.push_back({L"color_gray", L""});
    items.push_back({L"color_white", L""});
    items.push_back({L"color_custom", L""});
}

class HighlightCoverage {
public:
    HighlightCoverage(const Bitmap& bitmap, std::span<const POINT> points, int radius)
        : radius_(radius) {
        if (!bitmap.valid() || points.empty() || radius <= 0) {
            return;
        }

        std::int64_t minimum_x = points.front().x;
        std::int64_t minimum_y = points.front().y;
        std::int64_t maximum_x = points.front().x;
        std::int64_t maximum_y = points.front().y;
        for (const POINT point : points.subspan(1)) {
            minimum_x = std::min<std::int64_t>(minimum_x, point.x);
            minimum_y = std::min<std::int64_t>(minimum_y, point.y);
            maximum_x = std::max<std::int64_t>(maximum_x, point.x);
            maximum_y = std::max<std::int64_t>(maximum_y, point.y);
        }

        left_ = static_cast<int>(
            std::max<std::int64_t>(0, minimum_x - radius_));
        top_ = static_cast<int>(
            std::max<std::int64_t>(0, minimum_y - radius_));
        const int right = static_cast<int>(std::min<std::int64_t>(
            bitmap.width,
            maximum_x + static_cast<std::int64_t>(radius_) + 1));
        const int bottom = static_cast<int>(std::min<std::int64_t>(
            bitmap.height,
            maximum_y + static_cast<std::int64_t>(radius_) + 1));
        width_ = right - left_;
        height_ = bottom - top_;
        if (width_ <= 0 || height_ <= 0) {
            return;
        }

        const std::size_t row_width = static_cast<std::size_t>(width_);
        const std::size_t row_count = static_cast<std::size_t>(height_);
        if (row_count > std::numeric_limits<std::size_t>::max() / row_width) {
            width_ = 0;
            height_ = 0;
            allocation_failed_ = true;
            return;
        }
        try {
            pixels_.resize(row_width * row_count);
        } catch (const std::bad_alloc&) {
            width_ = 0;
            height_ = 0;
            allocation_failed_ = true;
        } catch (const std::length_error&) {
            width_ = 0;
            height_ = 0;
            allocation_failed_ = true;
        }
    }

    [[nodiscard]] bool valid() const noexcept {
        return width_ > 0 && height_ > 0 && !pixels_.empty();
    }

    [[nodiscard]] bool allocation_failed() const noexcept {
        return allocation_failed_;
    }

    void cover_line(POINT start, POINT end) noexcept {
        std::int64_t x = start.x;
        std::int64_t y = start.y;
        const std::int64_t target_x = end.x;
        const std::int64_t target_y = end.y;
        const std::int64_t dx = std::abs(target_x - x);
        const std::int64_t step_x = x < target_x ? 1 : -1;
        const std::int64_t dy = -std::abs(target_y - y);
        const std::int64_t step_y = y < target_y ? 1 : -1;
        std::int64_t error = dx + dy;

        while (true) {
            cover_circle(x, y);
            if (x == target_x && y == target_y) {
                break;
            }
            const std::int64_t doubled_error = error * 2;
            if (doubled_error >= dy) {
                error += dy;
                x += step_x;
            }
            if (doubled_error <= dx) {
                error += dx;
                y += step_y;
            }
        }
    }

    void blend(Bitmap& bitmap, COLORREF color, int alpha) const noexcept {
        alpha = std::clamp(alpha, 0, 255);
        if (!valid() || alpha == 0) {
            return;
        }
        const std::array<std::uint8_t, 3> source{
            GetBValue(color),
            GetGValue(color),
            GetRValue(color),
        };
        for (int local_y = 0; local_y < height_; ++local_y) {
            auto row = bitmap.row(top_ + local_y);
            const std::size_t coverage_offset =
                static_cast<std::size_t>(local_y) * static_cast<std::size_t>(width_);
            for (int local_x = 0; local_x < width_; ++local_x) {
                if (pixels_[coverage_offset + static_cast<std::size_t>(local_x)] == 0) {
                    continue;
                }
                auto* destination =
                    row.data() +
                    static_cast<std::size_t>(left_ + local_x) * Bitmap::bytes_per_pixel;
                for (std::size_t channel = 0; channel < source.size(); ++channel) {
                    destination[channel] = static_cast<std::uint8_t>(
                        (static_cast<int>(source[channel]) * alpha +
                         static_cast<int>(destination[channel]) * (255 - alpha) + 127) /
                        255);
                }
                destination[3] = 255;
            }
        }
    }

private:
    void cover_circle(std::int64_t center_x, std::int64_t center_y) noexcept {
        if (!valid()) {
            return;
        }
        const std::int64_t start_x =
            std::max<std::int64_t>(left_, center_x - radius_);
        const std::int64_t start_y =
            std::max<std::int64_t>(top_, center_y - radius_);
        const std::int64_t end_x =
            std::min<std::int64_t>(left_ + width_ - 1, center_x + radius_);
        const std::int64_t end_y =
            std::min<std::int64_t>(top_ + height_ - 1, center_y + radius_);
        const std::int64_t radius_squared =
            static_cast<std::int64_t>(radius_) * radius_;
        for (std::int64_t y = start_y; y <= end_y; ++y) {
            const std::int64_t delta_y = y - center_y;
            const std::size_t row_offset =
                static_cast<std::size_t>(y - top_) * static_cast<std::size_t>(width_);
            for (std::int64_t x = start_x; x <= end_x; ++x) {
                const std::int64_t delta_x = x - center_x;
                if (delta_x * delta_x + delta_y * delta_y <= radius_squared) {
                    pixels_[row_offset + static_cast<std::size_t>(x - left_)] = 255;
                }
            }
        }
    }

    int left_{};
    int top_{};
    int width_{};
    int height_{};
    int radius_{};
    bool allocation_failed_{};
    std::vector<std::uint8_t> pixels_;
};

[[nodiscard]] bool blend_highlight(Bitmap& bitmap, const Annotation& annotation) {
    const double requested_radius =
        std::isfinite(annotation.width)
            ? std::round(static_cast<double>(annotation.width) * 1.6)
            : 4.0;
    const int radius = static_cast<int>(
        std::clamp(requested_radius, 4.0, 4096.0));

    std::array<POINT, 2> endpoints{annotation.start, annotation.end};
    const std::span<const POINT> points =
        annotation.points.size() > 1
            ? std::span<const POINT>(annotation.points)
            : std::span<const POINT>(endpoints);
    HighlightCoverage coverage(bitmap, points, radius);
    if (!coverage.valid()) {
        return !coverage.allocation_failed();
    }

    for (std::size_t index = 1; index < points.size(); ++index) {
        coverage.cover_line(points[index - 1], points[index]);
    }
    if (points.size() == 1) {
        coverage.cover_line(points.front(), points.front());
    }
    coverage.blend(bitmap, annotation.color, annotation.alpha);
    return true;
}

class ScreenDc {
public:
    ScreenDc() noexcept : value_(GetDC(nullptr)) {}
    ~ScreenDc() {
        if (value_) {
            ReleaseDC(nullptr, value_);
        }
    }
    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;

    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HDC value_{};
};

class MemoryDc {
public:
    explicit MemoryDc(HDC compatible_with) noexcept : value_(CreateCompatibleDC(compatible_with)) {}
    ~MemoryDc() {
        if (value_) {
            DeleteDC(value_);
        }
    }
    MemoryDc(const MemoryDc&) = delete;
    MemoryDc& operator=(const MemoryDc&) = delete;

    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HDC value_{};
};

class OwnedGdiObject {
public:
    explicit OwnedGdiObject(HGDIOBJ value = nullptr) noexcept : value_(value) {}
    ~OwnedGdiObject() {
        if (value_) {
            DeleteObject(value_);
        }
    }
    OwnedGdiObject(const OwnedGdiObject&) = delete;
    OwnedGdiObject& operator=(const OwnedGdiObject&) = delete;

    [[nodiscard]] HGDIOBJ get() const noexcept { return value_; }

private:
    HGDIOBJ value_{};
};

class SelectedObject {
public:
    SelectedObject(HDC dc, HGDIOBJ object) noexcept
        : dc_(dc), previous_(dc && object ? SelectObject(dc, object) : nullptr) {}
    ~SelectedObject() {
        if (valid()) {
            SelectObject(dc_, previous_);
        }
    }
    SelectedObject(const SelectedObject&) = delete;
    SelectedObject& operator=(const SelectedObject&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return previous_ != nullptr && previous_ != HGDI_ERROR;
    }

private:
    HDC dc_{};
    HGDIOBJ previous_{};
};

void draw_polyline(HDC dc, const std::vector<POINT>& points) {
    if (points.empty()) {
        return;
    }
    MoveToEx(dc, points.front().x, points.front().y, nullptr);
    for (std::size_t index = 1; index < points.size(); ++index) {
        LineTo(dc, points[index].x, points[index].y);
    }
}

bool draw_text_with_style(
    HDC dc,
    const std::wstring& text,
    RECT bounds,
    COLORREF color,
    TextStyle style,
    HFONT font) {
    SelectedObject selected_font(dc, font);
    if (!selected_font.valid()) {
        return false;
    }
    SetBkMode(dc, TRANSPARENT);

    RECT measured = bounds;
    DrawTextW(
        dc,
        text.c_str(),
        static_cast<int>(text.size()),
        &measured,
        DT_LEFT | DT_TOP | DT_CALCRECT | DT_NOPREFIX);
    measured.right += 8;
    measured.bottom += 6;

    if (style == TextStyle::dark) {
        OwnedGdiObject background(CreateSolidBrush(RGB(31, 35, 41)));
        if (!background.get()) {
            return false;
        }
        FillRect(dc, &measured, reinterpret_cast<HBRUSH>(background.get()));
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(
            dc,
            text.c_str(),
            static_cast<int>(text.size()),
            &bounds,
            DT_LEFT | DT_TOP | DT_NOPREFIX);
    } else if (style == TextStyle::outline) {
        SetTextColor(dc, RGB(255, 255, 255));
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                RECT outline = bounds;
                OffsetRect(&outline, dx, dy);
                DrawTextW(
                    dc,
                    text.c_str(),
                    static_cast<int>(text.size()),
                    &outline,
                    DT_LEFT | DT_TOP | DT_NOPREFIX);
            }
        }
        SetTextColor(dc, color);
        DrawTextW(
            dc,
            text.c_str(),
            static_cast<int>(text.size()),
            &bounds,
            DT_LEFT | DT_TOP | DT_NOPREFIX);
    } else {
        SetTextColor(dc, color);
        DrawTextW(
            dc,
            text.c_str(),
            static_cast<int>(text.size()),
            &bounds,
            DT_LEFT | DT_TOP | DT_NOPREFIX);
    }

    return true;
}

bool blend_watermark(HDC compatible_dc,
                     std::uint8_t* target_bits,
                     const Annotation& annotation,
                     const AppConfig& config,
                     int width,
                     int height) {
    if (annotation.text.empty() || width <= 0 || height <= 0) {
        return true;
    }
    const int annotation_alpha = std::clamp(annotation.alpha, 0, 255);
    if (annotation_alpha == 0 || !compatible_dc || !target_bits) {
        return annotation_alpha == 0;
    }

    MemoryDc mask_dc(compatible_dc);
    if (!mask_dc.get()) {
        return false;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* mask_bits = nullptr;
    OwnedGdiObject mask_dib(
        CreateDIBSection(compatible_dc, &info, DIB_RGB_COLORS, &mask_bits, nullptr, 0));
    if (!mask_dib.get() || !mask_bits) {
        return false;
    }
    SelectedObject selected_bitmap(mask_dc.get(), mask_dib.get());
    if (!selected_bitmap.valid()) {
        return false;
    }
    const std::size_t byte_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * Bitmap::bytes_per_pixel;
    std::memset(mask_bits, 0, byte_count);

    OwnedGdiObject font(CreateFontW(-static_cast<int>(std::max(12.0F, annotation.width)),
                                    0,
                                    -180,
                                    -180,
                                    FW_NORMAL,
                                    FALSE,
                                    FALSE,
                                    FALSE,
                                    DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS,
                                    ANTIALIASED_QUALITY,
                                    DEFAULT_PITCH,
                                    config.text_font_family.c_str()));
    SelectedObject selected_font(mask_dc.get(), font.get());
    if (!font.get() || !selected_font.valid()) {
        return false;
    }
    SetBkMode(mask_dc.get(), TRANSPARENT);
    SetTextColor(mask_dc.get(), RGB(255, 255, 255));

    SIZE text_extent{};
    if (!GetTextExtentPoint32W(
            mask_dc.get(),
            annotation.text.c_str(),
            static_cast<int>(annotation.text.size()),
            &text_extent)) {
        return false;
    }
    const int step_x =
        std::max(120, static_cast<int>(text_extent.cx) + 72);
    const int step_y = std::max(70, static_cast<int>(annotation.width * 3.2F));
    for (int y = -step_y; y < height + step_y; y += step_y) {
        for (int x = -step_x; x < width + step_x; x += step_x) {
            if (!TextOutW(mask_dc.get(),
                          x,
                          y,
                          annotation.text.c_str(),
                          static_cast<int>(annotation.text.size()))) {
                return false;
            }
        }
    }
    if (!GdiFlush()) {
        return false;
    }

    const auto* mask = static_cast<const std::uint8_t*>(mask_bits);
    const std::uint8_t source_channels[]{
        GetBValue(annotation.color),
        GetGValue(annotation.color),
        GetRValue(annotation.color),
    };
    for (std::size_t offset = 0; offset < byte_count; offset += Bitmap::bytes_per_pixel) {
        const int coverage = std::max({mask[offset], mask[offset + 1], mask[offset + 2]});
        const int alpha = (coverage * annotation_alpha + 127) / 255;
        if (alpha == 0) {
            continue;
        }
        for (std::size_t channel_index = 0; channel_index < 3; ++channel_index) {
            target_bits[offset + channel_index] = static_cast<std::uint8_t>(
                (static_cast<int>(source_channels[channel_index]) * alpha +
                 static_cast<int>(target_bits[offset + channel_index]) * (255 - alpha) + 127) /
                255);
        }
        target_bits[offset + 3] = 255;
    }

    return true;
}

}  // namespace

void OverlaySession::build_toolbar() {
    toolbar_.clear();
    std::vector<std::pair<std::wstring, std::wstring>> items;
    items.push_back({L"drag", L""});

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
        if (id == L"watermark") return L"水";
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
        if (id == L"rect" || id == L"ellipse" || id == L"line" || id == L"arrow" ||
            id == L"pen" || id == L"text" || id == L"serial") {
            return 2;
        }
        if (id == L"mosaic" || id == L"blur" || id == L"highlight" || id == L"watermark" ||
            id == L"pin" || id == L"ocr" || id == L"select" || id == L"scroll" || id == L"eraser") {
            return 3;
        }
        if (id == L"undo" || id == L"redo") return 4;
        if (id == L"save" || id == L"close" || id == L"copy") return 5;
        return 0;
    };

    int last_category = -1;
    for (const auto& token : split_by_comma(request_.config.toolbar_order)) {
        if (token == L"blur") continue;
        if (token == L"ocr" && !request_.config.ocr_enabled) continue;
        if (token == L"lock" && !request_.config.annotation_enabled) continue;

        int cat = get_tool_category(token);
        const bool annotation_tool =
            token == L"select" || token == L"rect" || token == L"ellipse" || token == L"line" ||
            token == L"arrow" || token == L"pen" || token == L"mosaic" || token == L"blur" ||
            token == L"highlight" || token == L"watermark" || token == L"text" ||
            token == L"serial" || token == L"eraser" || token == L"undo" || token == L"redo";
        if (annotation_tool && !request_.config.annotation_enabled) continue;

        if (token == L"mosaic") {
            if (hidden(request_.config, L"mosaic") &&
                hidden(request_.config, L"blur")) {
                continue;
            }
        } else if (hidden(request_.config, token) && token != L"close") {
            continue;
        }

        std::wstring label = get_tool_label(token);
        if (label.empty()) continue;

        if (last_category != -1 && last_category != cat) {
            add_separator(items);
        }
        items.push_back({std::wstring(token), label});
        last_category = cat;
    }
    trim_trailing_separators(items);

    const RectI host_bounds = toolbar_host_bounds(monitors_, selection_, virtual_bounds_);
    const ToolbarMetrics metrics{40, 40, 4, 8};
    const auto rows = wrap_toolbar_items(items, metrics, host_bounds);
    const int total_width = toolbar_width(rows, metrics);
    const int total_height = toolbar_height(rows, metrics);
    if (rows.empty()) {
        toolbar_.clear();
        sub_toolbar_.clear();
        return;
    }

    int left = 0;
    int top = 0;
    if (toolbar_custom_origin_) {
        left = clamp_axis(
            toolbar_custom_origin_->x,
            total_width,
            host_bounds.left,
            host_bounds.right);
        top = clamp_axis(
            toolbar_custom_origin_->y,
            total_height,
            host_bounds.top,
            host_bounds.bottom);
        *toolbar_custom_origin_ = POINT{left, top};
    } else {
        const int preferred_left = selection_.right - total_width;
        left = clamp_axis(
            preferred_left,
            total_width,
            host_bounds.left,
            host_bounds.right);
        const int below_top = selection_.bottom + 8;
        const int above_top = selection_.top - total_height - 8;
        top = below_top;
        if (below_top + total_height > host_bounds.bottom &&
            above_top >= host_bounds.top) {
            top = above_top;
        }
        top = clamp_axis(
            top,
            total_height,
            host_bounds.top,
            host_bounds.bottom);
    }

    place_toolbar_rows(toolbar_, rows, metrics, left, top);
    for (auto& button : toolbar_) {
        if (button.id == L"undo" && !annotation_history_.can_undo()) {
            button.enabled = false;
            button.disabled_reason = L"暂无可撤销操作";
        } else if (button.id == L"redo" && !annotation_history_.can_redo()) {
            button.enabled = false;
            button.disabled_reason = L"暂无可重做操作";
        } else if (
            button.id == L"scroll" &&
            (selection_.width() < 64 || selection_.height() < 64)) {
            button.enabled = false;
            button.disabled_reason = L"选区至少需要 64 × 64 像素";
        } else if (button.id == L"scroll" && !annotations_.empty()) {
            button.enabled = false;
            button.disabled_reason = L"请先撤销标注再开始长截图";
        }
    }
    build_sub_toolbar();
}

void OverlaySession::build_sub_toolbar() {
    sub_toolbar_.clear();
    if (active_tool_ == Tool::none) {
        return;
    }
    Tool style_tool = active_tool_;
    const bool editing_selected =
        active_tool_ == Tool::select &&
        selected_annotation_idx_ >= 0 &&
        selected_annotation_idx_ < static_cast<int>(annotations_.size());
    if (editing_selected) {
        style_tool =
            annotations_[static_cast<std::size_t>(selected_annotation_idx_)].tool;
    }
    std::vector<std::pair<std::wstring, std::wstring>> items;

    if (style_tool == Tool::mosaic || style_tool == Tool::blur) {
        const bool mosaic_available =
            !hidden(request_.config, L"mosaic");
        const bool blur_available =
            !hidden(request_.config, L"blur");
        if (mosaic_available && blur_available) {
            items.push_back({L"effect_mosaic", L"马赛克"});
            items.push_back({L"effect_blur", L"模糊"});
        }
        if (!editing_selected) {
            items.push_back({L"|", L""});
            items.push_back({L"mode_smear", L"涂抹"});
            items.push_back({L"mode_rect", L"框选"});
        }
        items.push_back({L"|", L""});
        items.push_back({L"mosaic_strength_slider", L""});
        items.push_back({L"|", L""});
        items.push_back({L"width_small", L""});
        items.push_back({L"width_medium", L""});
        items.push_back({L"width_large", L""});
    } else if (style_tool == Tool::text) {
        items.push_back({L"text_style_normal", L""});
        items.push_back({L"text_style_dark", L""});
        items.push_back({L"text_style_outline", L""});
        items.push_back({L"|", L""});
        std::wstring size_label = text_size_label(active_text_size_);
        items.push_back({L"text_size_btn", size_label});
        items.push_back({L"|", L""});
        add_feishu_palette(items);
    } else if (style_tool == Tool::watermark) {
        items.push_back({L"watermark_text", watermark_text_.empty() ? L"水印文字" : watermark_text_});
        items.push_back({L"watermark_apply", L"应用"});
        items.push_back({L"watermark_clear", L"清除"});
        items.push_back({L"|", L""});
        items.push_back({L"text_size_btn", text_size_label(active_text_size_)});
        items.push_back({L"|", L""});
        items.push_back({L"watermark_opacity_slider", L""});
        items.push_back({L"|", L""});
        add_feishu_palette(items);
    } else if (tool_is_feishu_style_shape(style_tool)) {
        items.push_back({L"width_small", L""});
        items.push_back({L"width_medium", L""});
        items.push_back({L"width_large", L""});
        items.push_back({L"|", L""});
        add_feishu_palette(items);
    } else {
        if (tool_supports_color(style_tool)) {
            add_feishu_palette(items);
        }
        if (tool_supports_width(style_tool)) {
            if (!items.empty()) {
                items.push_back({L"|", L""});
            }
            items.push_back({L"width_small", L""});
            items.push_back({L"width_medium", L""});
            items.push_back({L"width_large", L""});
        }
        if (tool_supports_alpha(style_tool)) {
            if (!items.empty()) {
                items.push_back({L"|", L""});
            }
            items.push_back({L"alpha_low", L""});
            items.push_back({L"alpha_medium", L""});
            items.push_back({L"alpha_high", L""});
        }
    }
    if (items.empty()) {
        return;
    }

    if (toolbar_.empty()) return;
    const RectI host_bounds = toolbar_host_bounds(monitors_, selection_, virtual_bounds_);
    const ToolbarMetrics metrics{36, 34, 4, 8};
    const auto rows = wrap_toolbar_items(items, metrics, host_bounds);
    const int total_width = toolbar_width(rows, metrics);
    const int total_height = toolbar_height(rows, metrics);
    if (rows.empty()) {
        return;
    }

    const RectI anchor = buttons_bounds(toolbar_);
    const int left =
        clamp_axis(anchor.left - metrics.padding, total_width, host_bounds.left, host_bounds.right);
    const bool main_above = anchor.top < selection_.top;
    const int preferred_top = main_above ? anchor.top - total_height - 6 : anchor.bottom + 6;
    const int fallback_top = main_above ? anchor.bottom + 6 : anchor.top - total_height - 6;
    int top = preferred_top;
    if ((top < host_bounds.top || top + total_height > host_bounds.bottom) &&
        fallback_top >= host_bounds.top && fallback_top + total_height <= host_bounds.bottom) {
        top = fallback_top;
    }
    top = clamp_axis(top, total_height, host_bounds.top, host_bounds.bottom);

    place_toolbar_rows(sub_toolbar_, rows, metrics, left, top);
    const bool has_watermark = std::ranges::any_of(
        annotations_,
        [](const Annotation& annotation) {
            return annotation.tool == Tool::watermark;
        });
    for (auto& button : sub_toolbar_) {
        if (button.id == L"watermark_clear" && !has_watermark) {
            button.enabled = false;
            button.disabled_reason = L"当前没有已应用的水印";
        }
    }
}

void OverlaySession::invoke_sub(std::wstring_view id, HWND source) {
    if (id == L"color_red") {
        active_color_ = RGB(245, 34, 45);
    } else if (id == L"color_green") {
        active_color_ = RGB(82, 196, 26);
    } else if (id == L"color_blue") {
        active_color_ = RGB(0, 102, 255);
    } else if (id == L"color_yellow") {
        active_color_ = RGB(250, 219, 20);
    } else if (id == L"color_black") {
        active_color_ = RGB(0, 0, 0);
    } else if (id == L"color_gray") {
        active_color_ = RGB(143, 149, 158);
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
        begin_annotation_transaction();
        show_rgb_picker_popup(
            source,
            custom_color_,
            button_bounds,
            virtual_bounds_,
            [this](COLORREF new_color) {
                if (done_) {
                    return;
                }
                custom_color_ = new_color;
                active_color_ = new_color;
                request_.config.custom_color = format_hex_color(new_color);
                apply_active_style_to_selected();
                invalidate_all();
            },
            [this] {
                if (done_) {
                    return;
                }
                commit_annotation_transaction();
                build_toolbar();
                build_sub_toolbar();
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
    } else if (id == L"text_style_normal") {
        active_text_style_ = TextStyle::normal;
    } else if (id == L"text_style_dark") {
        active_text_style_ = TextStyle::dark;
    } else if (id == L"text_style_outline") {
        active_text_style_ = TextStyle::outline;
    } else if (id == L"text_size_btn") {
        text_size_dropdown_open_ = !text_size_dropdown_open_;
    } else if (id == L"watermark_text") {
        if (prompt_window_ && IsWindow(prompt_window_)) {
            SetForegroundWindow(prompt_window_);
            return;
        }
        POINT position{};
        for (const auto& button : sub_toolbar_) {
            if (button.id == L"watermark_text") {
                position = {button.bounds.left, button.bounds.bottom + 6};
                break;
            }
        }
        bool is_light = should_use_light_theme(request_.config.theme);
        prompt_window_ = show_text_prompt(
            source,
            position,
            active_color_,
            active_text_size_,
            is_light,
            [this](std::optional<std::wstring> text) {
                prompt_window_ = nullptr;
                if (!done_ && text) {
                    watermark_text_ = std::move(*text);
                    invalidate_all();
                }
            },
            watermark_text_);
    } else if (id == L"watermark_apply") {
        apply_watermark();
    } else if (id == L"watermark_clear") {
        record_annotation_change();
        std::erase_if(annotations_, [](const Annotation& annotation) {
            return annotation.tool == Tool::watermark;
        });
        renumber_serial_annotations(annotations_);
        build_toolbar();
        build_sub_toolbar();
    } else if (id == L"effect_mosaic") {
        mosaic_is_blur_ = false;
        if (active_tool_ != Tool::select) {
            active_tool_ = Tool::mosaic;
            preview_.tool = Tool::mosaic;
        }
        build_sub_toolbar();
    } else if (id == L"effect_blur") {
        mosaic_is_blur_ = true;
        if (active_tool_ != Tool::select) {
            active_tool_ = Tool::blur;
            preview_.tool = Tool::blur;
        }
        build_sub_toolbar();
    } else if (id == L"mode_smear") {
        mosaic_is_rect_ = false;
        build_sub_toolbar();
    } else if (id == L"mode_rect") {
        mosaic_is_rect_ = true;
        preview_.points.clear();
        build_sub_toolbar();
    }
    apply_active_style_to_selected();
}

void OverlaySession::invoke(std::wstring_view id, HWND source) {
    if (ocr_running_) {
        if (id == L"close") {
            cancel_ocr();
        }
        return;
    }
    if (id == L"lock") {
        request_.config.annotation_locked_tool = !request_.config.annotation_locked_tool;
    } else if (id == L"mosaic") {
        const bool mosaic_available =
            !hidden(request_.config, L"mosaic");
        const bool blur_available =
            !hidden(request_.config, L"blur");
        const Tool target =
            !mosaic_available && blur_available
                ? Tool::blur
                : (mosaic_is_blur_ && blur_available
                       ? Tool::blur
                       : Tool::mosaic);
        const bool switching_on = active_tool_ != target;
        active_tool_ = switching_on ? target : Tool::none;
        selected_annotation_idx_ = -1;
    } else if (id == L"watermark") {
        const bool switching_on = active_tool_ != Tool::watermark;
        active_tool_ = switching_on ? Tool::watermark : Tool::none;
        selected_annotation_idx_ = -1;
        if (switching_on && active_color_ == RGB(245, 34, 45)) {
            active_color_ = RGB(255, 150, 150);
        }
    } else if (const Tool tool = tool_from_id(id); tool != Tool::none) {
        const bool switching_on = active_tool_ != tool;
        active_tool_ = switching_on ? tool : Tool::none;
        selected_annotation_idx_ = -1;
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

void OverlaySession::apply_watermark() {
    if (selection_.empty()) {
        return;
    }
    record_annotation_change();
    std::erase_if(annotations_, [](const Annotation& annotation) {
        return annotation.tool == Tool::watermark;
    });

    Annotation annotation;
    annotation.tool = Tool::watermark;
    annotation.start = {0, 0};
    annotation.end = {selection_.width(), selection_.height()};
    annotation.text = watermark_text_.empty() ? L"Air Screenshot" : watermark_text_;
    annotation.color = active_color_;
    annotation.width = active_text_size_;
    annotation.alpha = std::clamp(
        static_cast<int>(
            std::lround(watermark_opacity_ * 255.0 / 100.0)),
        0,
        255);
    annotation.text_style = active_text_style_;
    annotations_.push_back(std::move(annotation));
    finish_annotation();
}

void OverlaySession::finish_annotation() {
    if (!request_.config.annotation_locked_tool) {
        active_tool_ = Tool::none;
    }
    build_toolbar();
    build_sub_toolbar();
    invalidate_all();
}

void OverlaySession::record_annotation_change() {
    if (annotation_transaction_before_) {
        mark_annotation_transaction_changed();
        return;
    }
    annotation_history_.record(annotations_);
    mark_annotation_visual_changed();
}

void OverlaySession::mark_annotation_visual_changed() noexcept {
    if (annotation_revision_ == std::numeric_limits<std::uint64_t>::max()) {
        annotation_revision_ = 1;
    } else {
        ++annotation_revision_;
    }
}

void OverlaySession::begin_annotation_transaction() {
    if (annotation_transaction_before_) {
        return;
    }
    annotation_transaction_before_ = annotations_;
    annotation_transaction_changed_ = false;
}

void OverlaySession::mark_annotation_transaction_changed() noexcept {
    if (annotation_transaction_before_) {
        annotation_transaction_changed_ = true;
        mark_annotation_visual_changed();
    }
}

void OverlaySession::commit_annotation_transaction() {
    if (!annotation_transaction_before_) {
        return;
    }
    if (annotation_transaction_changed_) {
        annotation_history_.record(std::move(*annotation_transaction_before_));
    }
    annotation_transaction_before_.reset();
    annotation_transaction_changed_ = false;
}

void OverlaySession::cancel_annotation_transaction() {
    if (!annotation_transaction_before_) {
        return;
    }
    if (annotation_transaction_changed_) {
        annotations_ = std::move(*annotation_transaction_before_);
        mark_annotation_visual_changed();
    }
    annotation_transaction_before_.reset();
    annotation_transaction_changed_ = false;
}

void OverlaySession::sync_active_style_from_selected() {
    if (selected_annotation_idx_ < 0 ||
        selected_annotation_idx_ >= static_cast<int>(annotations_.size())) {
        return;
    }
    const Annotation& annotation =
        annotations_[static_cast<std::size_t>(selected_annotation_idx_)];
    if (tool_supports_color(annotation.tool)) {
        active_color_ = annotation.color;
    }
    if (tool_supports_width(annotation.tool)) {
        active_width_ = annotation.width;
    }
    if (annotation.tool == Tool::text) {
        active_text_size_ = nearest_text_size(annotation.width);
        active_text_style_ = annotation.text_style;
    } else if (annotation.tool == Tool::highlight) {
        active_highlight_alpha_ = std::clamp(annotation.alpha, 24, 192);
    } else if (annotation.tool == Tool::mosaic ||
               annotation.tool == Tool::blur) {
        mosaic_is_blur_ = annotation.tool == Tool::blur;
        mosaic_is_rect_ = annotation.points.empty();
        mosaic_strength_ = std::clamp(annotation.alpha, 0, 100);
    }
}

void OverlaySession::apply_active_style_to_selected() {
    if (active_tool_ != Tool::select ||
        selected_annotation_idx_ < 0 ||
        selected_annotation_idx_ >= static_cast<int>(annotations_.size())) {
        return;
    }

    const std::size_t index =
        static_cast<std::size_t>(selected_annotation_idx_);
    Annotation updated = annotations_[index];
    if (tool_supports_color(updated.tool)) {
        updated.color = active_color_;
    }
    if (tool_supports_width(updated.tool)) {
        updated.width = active_width_;
    }
    if (updated.tool == Tool::text) {
        updated.width = active_text_size_;
        updated.text_style = active_text_style_;
    } else if (updated.tool == Tool::highlight) {
        updated.alpha = active_highlight_alpha_;
    } else if (updated.tool == Tool::mosaic ||
               updated.tool == Tool::blur) {
        updated.tool = mosaic_is_blur_ ? Tool::blur : Tool::mosaic;
        updated.alpha = mosaic_strength_;
    }

    const Annotation& current = annotations_[index];
    const bool changed =
        updated.tool != current.tool ||
        updated.color != current.color ||
        updated.width != current.width ||
        updated.alpha != current.alpha ||
        updated.text_style != current.text_style;
    if (!changed) {
        return;
    }
    record_annotation_change();
    annotations_[index] = std::move(updated);
    build_toolbar();
    build_sub_toolbar();
    invalidate_all();
}

void OverlaySession::edit_selected_text(HWND source) {
    if (active_tool_ != Tool::select ||
        selected_annotation_idx_ < 0 ||
        selected_annotation_idx_ >= static_cast<int>(annotations_.size()) ||
        annotations_[static_cast<std::size_t>(selected_annotation_idx_)].tool !=
            Tool::text) {
        return;
    }
    if (prompt_window_ && IsWindow(prompt_window_)) {
        SetForegroundWindow(prompt_window_);
        return;
    }

    const std::size_t index =
        static_cast<std::size_t>(selected_annotation_idx_);
    const Annotation existing = annotations_[index];
    const POINT position{
        selection_.left + existing.start.x,
        selection_.top + existing.start.y,
    };
    const bool is_light = should_use_light_theme(request_.config.theme);
    prompt_window_ = show_text_prompt(
        source,
        position,
        existing.color,
        existing.width,
        is_light,
        [this, index](std::optional<std::wstring> text) {
            prompt_window_ = nullptr;
            if (done_ || !text || text->empty() ||
                index >= annotations_.size() ||
                annotations_[index].tool != Tool::text ||
                annotations_[index].text == *text) {
                return;
            }
            record_annotation_change();
            annotations_[index].text = std::move(*text);
            selected_annotation_idx_ = static_cast<int>(index);
            sync_active_style_from_selected();
            build_toolbar();
            build_sub_toolbar();
            invalidate_all();
        },
        existing.text);
}

void OverlaySession::undo() {
    cancel_annotation_transaction();
    if (annotation_history_.undo(annotations_)) {
        renumber_serial_annotations(annotations_);
        selected_annotation_idx_ = -1;
        mark_annotation_visual_changed();
    }
    build_toolbar();
    build_sub_toolbar();
    invalidate_all();
}

void OverlaySession::redo() {
    cancel_annotation_transaction();
    if (annotation_history_.redo(annotations_)) {
        renumber_serial_annotations(annotations_);
        selected_annotation_idx_ = -1;
        mark_annotation_visual_changed();
    }
    build_toolbar();
    build_sub_toolbar();
    invalidate_all();
}

Bitmap OverlaySession::original_selection() const {
    return compose_selection(monitors_, selection_);
}

const Bitmap& OverlaySession::cached_rendered_selection() const {
    const bool same_selection =
        selection_.left == rendered_selection_cache_bounds_.left &&
        selection_.top == rendered_selection_cache_bounds_.top &&
        selection_.right == rendered_selection_cache_bounds_.right &&
        selection_.bottom == rendered_selection_cache_bounds_.bottom;
    if (!rendered_selection_cache_.valid() ||
        rendered_selection_cache_revision_ != annotation_revision_ ||
        !same_selection) {
        Bitmap next =
            render_annotations(original_selection(), annotations_, request_.config);
        if (next.valid()) {
            rendered_selection_cache_ = std::move(next);
            rendered_selection_cache_revision_ = annotation_revision_;
            rendered_selection_cache_bounds_ = selection_;
        } else {
            rendered_selection_cache_ = {};
        }
    }
    return rendered_selection_cache_;
}

Bitmap render_annotations(Bitmap result,
                          const std::vector<Annotation>& annotations,
                          const AppConfig& config) {
    if (!result.valid()) {
        return {};
    }

    ScreenDc screen;
    if (!screen.get()) {
        return {};
    }
    MemoryDc dc(screen.get());
    if (!dc.get()) {
        return {};
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = result.width;
    info.bmiHeader.biHeight = -result.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    OwnedGdiObject dib(
        CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!dib.get() || !bits) {
        return {};
    }
    SelectedObject selected_bitmap(dc.get(), dib.get());
    if (!selected_bitmap.valid()) {
        return {};
    }
    std::memcpy(bits, result.pixels.data(), result.pixels.size());
    SetBkMode(dc.get(), TRANSPARENT);
    SelectedObject selected_brush(dc.get(), GetStockObject(HOLLOW_BRUSH));
    if (!selected_brush.valid()) {
        return {};
    }
    const auto sync_from_dib = [&]() {
        if (!GdiFlush()) {
            return false;
        }
        std::memcpy(result.pixels.data(), bits, result.pixels.size());
        return true;
    };
    const auto sync_to_dib = [&]() {
        std::memcpy(bits, result.pixels.data(), result.pixels.size());
    };

    for (const auto& annotation : annotations) {
        if (annotation.tool == Tool::mosaic || annotation.tool == Tool::blur ||
            annotation.tool == Tool::highlight) {
            if (!sync_from_dib()) {
                return {};
            }
            if (annotation.tool == Tool::mosaic) {
                const int strength = std::clamp(annotation.alpha, 0, 100);
                const int block_size = std::clamp(4 + strength / 8, 4, 18);
                if (annotation.points.empty()) {
                    pixelate_rect(
                        result,
                        RectI{
                            annotation.start.x,
                            annotation.start.y,
                            annotation.end.x,
                            annotation.end.y,
                        },
                        block_size);
                } else {
                    for (const POINT point : annotation.points) {
                        const int radius =
                            std::max(
                                5,
                                static_cast<int>(
                                    annotation.width * 3.5F));
                        pixelate_circle(result, point, radius, block_size);
                    }
                }
            } else if (annotation.tool == Tool::blur) {
                const int strength = std::clamp(annotation.alpha, 0, 100);
                const int rect_radius = std::clamp(3 + strength / 5, 3, 23);
                const int circle_radius = std::clamp(2 + strength / 12, 2, 10);
                if (annotation.points.empty()) {
                    blur_rect(
                        result,
                        RectI{
                            annotation.start.x,
                            annotation.start.y,
                            annotation.end.x,
                            annotation.end.y,
                        },
                        rect_radius);
                } else {
                    for (const POINT point : annotation.points) {
                        const int radius =
                            std::max(5, static_cast<int>(annotation.width * 3.5F));
                        blur_circle(result, point, radius, circle_radius);
                    }
                }
            } else {
                if (!blend_highlight(result, annotation)) {
                    return {};
                }
            }
            sync_to_dib();
            continue;
        }
        if (annotation.tool == Tool::text) {
            OwnedGdiObject text_font(
                CreateFontW(static_cast<int>(annotation.width),
                            0,
                            0,
                            0,
                            config.text_font_bold ? FW_BOLD : FW_NORMAL,
                            config.text_font_italic ? TRUE : FALSE,
                            FALSE,
                            FALSE,
                            DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY,
                            DEFAULT_PITCH,
                            config.text_font_family.c_str()));
            if (!text_font.get()) {
                return {};
            }
            RECT text_rect{
                annotation.start.x,
                annotation.start.y,
                result.width,
                result.height,
            };
            if (!draw_text_with_style(dc.get(),
                                      annotation.text,
                                      text_rect,
                                      annotation.color,
                                      annotation.text_style,
                                      reinterpret_cast<HFONT>(text_font.get()))) {
                return {};
            }
            continue;
        }
        if (annotation.tool == Tool::serial) {
            const int radius = static_cast<int>(std::round(8.0F + annotation.width * 1.5F));
            OwnedGdiObject fill_brush(CreateSolidBrush(annotation.color));
            SelectedObject selected_fill(dc.get(), fill_brush.get());
            OwnedGdiObject white_pen(CreatePen(PS_SOLID, 1, RGB(255, 255, 255)));
            SelectedObject selected_white_pen(dc.get(), white_pen.get());
            if (!fill_brush.get() || !selected_fill.valid() || !white_pen.get() ||
                !selected_white_pen.valid()) {
                return {};
            }

            Ellipse(dc.get(),
                    annotation.start.x - radius,
                    annotation.start.y - radius,
                    annotation.start.x + radius,
                    annotation.start.y + radius);

            const std::wstring serial_text = std::to_wstring(annotation.serial);
            RECT text_rect{
                annotation.start.x - radius,
                annotation.start.y - radius,
                annotation.start.x + radius,
                annotation.start.y + radius,
            };
            const int font_height = static_cast<int>(std::round(radius * 1.0F));
            OwnedGdiObject serial_font(CreateFontW(font_height,
                                                   0,
                                                   0,
                                                   0,
                                                   FW_BOLD,
                                                   FALSE,
                                                   FALSE,
                                                   FALSE,
                                                   DEFAULT_CHARSET,
                                                   OUT_DEFAULT_PRECIS,
                                                   CLIP_DEFAULT_PRECIS,
                                                   CLEARTYPE_QUALITY,
                                                   DEFAULT_PITCH,
                                                   L"Consolas"));
            SelectedObject selected_serial_font(dc.get(), serial_font.get());
            if (!serial_font.get() || !selected_serial_font.valid()) {
                return {};
            }
            SetTextColor(dc.get(), RGB(255, 255, 255));
            DrawTextW(dc.get(),
                      serial_text.c_str(),
                      static_cast<int>(serial_text.size()),
                      &text_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            continue;
        }
        if (annotation.tool == Tool::watermark) {
            if (!GdiFlush() ||
                !blend_watermark(
                    dc.get(),
                    static_cast<std::uint8_t*>(bits),
                    annotation,
                    config,
                    result.width,
                    result.height)) {
                return {};
            }
            continue;
        }
        if (annotation.tool != Tool::rectangle && annotation.tool != Tool::ellipse &&
            annotation.tool != Tool::line && annotation.tool != Tool::arrow &&
            annotation.tool != Tool::pen) {
            continue;
        }

        const int pen_width =
            std::clamp(static_cast<int>(std::lround(annotation.width)), 1, 1024);
        LOGBRUSH pen_brush{};
        pen_brush.lbStyle = BS_SOLID;
        pen_brush.lbColor = annotation.color;
        HPEN native_pen = ExtCreatePen(
            PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
            static_cast<DWORD>(pen_width),
            &pen_brush,
            0,
            nullptr);
        if (!native_pen) {
            native_pen = CreatePen(PS_SOLID, pen_width, annotation.color);
        }
        OwnedGdiObject pen(native_pen);
        SelectedObject selected_pen(dc.get(), pen.get());
        SelectedObject selected_hollow_brush(
            dc.get(),
            GetStockObject(NULL_BRUSH));
        if (!pen.get() || !selected_pen.valid() ||
            !selected_hollow_brush.valid()) {
            return {};
        }
        SetTextColor(dc.get(), annotation.color);

        if (annotation.tool == Tool::rectangle) {
            const RectI bounds{annotation.start.x, annotation.start.y, annotation.end.x, annotation.end.y};
            const RectI normalized = bounds.normalized();
            Rectangle(dc.get(), normalized.left, normalized.top, normalized.right, normalized.bottom);
        } else if (annotation.tool == Tool::ellipse) {
            const RectI bounds{annotation.start.x, annotation.start.y, annotation.end.x, annotation.end.y};
            const RectI normalized = bounds.normalized();
            Ellipse(dc.get(), normalized.left, normalized.top, normalized.right, normalized.bottom);
        } else if (annotation.tool == Tool::line) {
            MoveToEx(dc.get(), annotation.start.x, annotation.start.y, nullptr);
            LineTo(dc.get(), annotation.end.x, annotation.end.y);
        } else if (annotation.tool == Tool::arrow) {
            MoveToEx(dc.get(), annotation.start.x, annotation.start.y, nullptr);
            LineTo(dc.get(), annotation.end.x, annotation.end.y);
            const double angle = std::atan2(
                static_cast<double>(annotation.end.y - annotation.start.y),
                static_cast<double>(annotation.end.x - annotation.start.x));
            const double length = 10.0 + annotation.width * 2.0;
            for (double offset : {0.45, -0.45}) {
                MoveToEx(dc.get(), annotation.end.x, annotation.end.y, nullptr);
                LineTo(dc.get(),
                       annotation.end.x - static_cast<int>(std::cos(angle + offset) * length),
                       annotation.end.y - static_cast<int>(std::sin(angle + offset) * length));
            }
        } else if (annotation.tool == Tool::pen) {
            if (annotation.points.size() == 1) {
                const int radius = std::max(1, (pen_width + 1) / 2);
                OwnedGdiObject point_brush(
                    CreateSolidBrush(annotation.color));
                SelectedObject selected_point_brush(
                    dc.get(),
                    point_brush.get());
                if (!point_brush.get() || !selected_point_brush.valid()) {
                    return {};
                }
                const POINT point = annotation.points.front();
                Ellipse(
                    dc.get(),
                    point.x - radius,
                    point.y - radius,
                    point.x + radius + 1,
                    point.y + radius + 1);
            } else {
                draw_polyline(dc.get(), annotation.points);
            }
        }
    }
    if (!sync_from_dib()) {
        return {};
    }
    result.make_opaque();
    return result;
}

Bitmap OverlaySession::rendered_selection() const {
    return cached_rendered_selection();
}

void OverlaySession::complete_clipboard() {
    Bitmap rendered = rendered_selection();
    if (rendered.empty()) {
        show_output_error(nullptr, L"无法生成截图图像。截图和标注仍会保留，您可以调整后重试。");
        return;
    }
    std::wstring error;
    if (!copy_bitmap_to_clipboard(rendered, &error)) {
        show_output_error(
            nullptr,
            error.empty()
                ? L"无法复制到剪贴板。截图和标注仍会保留，您可以重试或改为保存文件。"
                : error + L"\n\n截图和标注仍会保留，您可以重试或改为保存文件。");
        return;
    }
    finish({ExitCode::success, L"截图已复制到剪贴板。"});
}

void OverlaySession::complete_default(HWND owner) {
    if (_wcsicmp(request_.config.default_output.c_str(), L"file") == 0) {
        complete_file({}, owner);
    } else {
        complete_clipboard();
    }
}

void OverlaySession::complete_file(std::wstring_view requested_path, HWND owner) {
    std::optional<std::filesystem::path> path;
    if (requested_path.empty() && request_.action == RegionAction::interactive) {
        enter_modal();
        path = prompt_png_path(owner);
        if (done_) {
            leave_modal();
            return;
        }
        leave_modal();
        if (!path) {
            return;
        }
    } else {
        path = resolve_output_path(requested_path);
    }
    Bitmap rendered = rendered_selection();
    if (rendered.empty()) {
        show_output_error(owner, L"无法生成截图图像。截图和标注仍会保留，您可以调整后重试。");
        return;
    }
    std::wstring error;
    if (!save_png(rendered, *path, &error)) {
        show_output_error(
            owner,
            error.empty()
                ? L"无法保存截图。截图和标注仍会保留，您可以重新选择保存位置。"
                : error + L"\n\n截图和标注仍会保留，您可以重新选择保存位置。");
        return;
    }
    RegionResult result{ExitCode::success, L"截图已保存。"};
    result.path = path->wstring();
    finish(std::move(result));
}

void OverlaySession::complete_ocr() {
    if (!request_.config.ocr_enabled) {
        show_output_error(nullptr, L"OCR 模块已关闭。您可以在设置中启用后重试。");
        return;
    }
    if (ocr_running_) {
        return;
    }

    if (request_.copy_ocr && !pending_ocr_text_.empty()) {
        std::wstring error;
        if (!copy_text_to_clipboard(pending_ocr_text_, &error)) {
            show_output_error(
                nullptr,
                (error.empty() ? L"无法复制识别结果。" : error) +
                    std::wstring(L"\n\n识别结果仍会保留，再次点击 OCR 可重试复制。"));
            return;
        }
        RegionResult result{ExitCode::success, std::wstring(strings::ocr_success)};
        result.text = std::move(pending_ocr_text_);
        finish(std::move(result));
        return;
    }

    Bitmap bitmap = original_selection();
    if (bitmap.empty()) {
        show_output_error(nullptr, L"无法生成 OCR 图像。截图和标注仍会保留，您可以调整选区后重试。");
        return;
    }

    const AppConfig config = request_.config;
    const HWND notification_window = modal_owner();
    {
        std::scoped_lock lock(ocr_mutex_);
        ocr_completion_.reset();
    }
    ocr_running_ = true;
    ocr_cancelling_ = false;
    invalidate_all();

    try {
        ocr_thread_ = std::jthread(
            [this,
             bitmap = std::move(bitmap),
             config,
             notification_window](std::stop_token stop_token) mutable {
                OcrOutput output;
                try {
                    output = recognize_text(bitmap, config, stop_token);
                } catch (const std::exception& exception) {
                    output = {
                        false,
                        {},
                        L"OCR 识别异常：" + from_utf8(exception.what()),
                    };
                } catch (...) {
                    output = {false, {}, L"OCR 识别发生未知异常。"};
                }

                OcrCompletion completion{
                    std::move(output),
                    stop_token.stop_requested(),
                };
                {
                    std::scoped_lock lock(ocr_mutex_);
                    ocr_completion_ = std::move(completion);
                }
                if (notification_window && IsWindow(notification_window)) {
                    PostMessageW(
                        notification_window,
                        kOverlayOcrCompletedMessage,
                        0,
                        0);
                }
            });
    } catch (const std::exception& exception) {
        ocr_running_ = false;
        ocr_cancelling_ = false;
        show_output_error(
            nullptr,
            L"无法启动后台 OCR：" + from_utf8(exception.what()));
    } catch (...) {
        ocr_running_ = false;
        ocr_cancelling_ = false;
        show_output_error(nullptr, L"无法启动后台 OCR。");
    }
}

void OverlaySession::cancel_ocr() {
    if (!ocr_running_) {
        return;
    }
    ocr_cancelling_ = true;
    if (ocr_thread_.joinable()) {
        ocr_thread_.request_stop();
    }
    invalidate_all();
}

void OverlaySession::handle_ocr_completion() {
    std::optional<OcrCompletion> completion;
    {
        std::scoped_lock lock(ocr_mutex_);
        if (!ocr_completion_) {
            return;
        }
        completion = std::move(ocr_completion_);
        ocr_completion_.reset();
    }
    if (ocr_thread_.joinable()) {
        ocr_thread_.join();
    }
    ocr_running_ = false;
    ocr_cancelling_ = false;

    if (done_ || completion->cancelled) {
        invalidate_all();
        return;
    }
    if (!completion->output.ok) {
        show_output_error(
            nullptr,
            completion->output.error.empty()
                ? L"OCR 识别失败。截图和标注仍会保留，您可以重试。"
                : completion->output.error +
                      L"\n\n截图和标注仍会保留，您可以重试。");
        return;
    }

    if (request_.copy_ocr) {
        std::wstring error;
        if (!copy_text_to_clipboard(completion->output.text, &error)) {
            pending_ocr_text_ = std::move(completion->output.text);
            show_output_error(
                nullptr,
                (error.empty() ? L"无法复制识别结果。" : error) +
                    std::wstring(L"\n\n识别结果仍会保留，再次点击 OCR 可重试复制。"));
            return;
        }
    }
    RegionResult result{ExitCode::success, request_.copy_ocr ? std::wstring(strings::ocr_success) : L"OCR 完成。"};
    result.text = std::move(completion->output.text);
    finish(std::move(result));
}

void OverlaySession::stop_ocr_worker() {
    if (ocr_thread_.joinable()) {
        ocr_thread_.request_stop();
        ocr_thread_.join();
    }
    {
        std::scoped_lock lock(ocr_mutex_);
        ocr_completion_.reset();
    }
    ocr_running_ = false;
    ocr_cancelling_ = false;
}

void OverlaySession::complete_pin() {
    Bitmap rendered = rendered_selection();
    if (rendered.empty()) {
        show_output_error(nullptr, L"无法生成贴图图像。截图和标注仍会保留，您可以调整后重试。");
        return;
    }
    RegionResult result{ExitCode::success, L"贴图已创建。"};
    result.action = RegionAction::pin;
    result.bitmap = std::move(rendered);
    finish(std::move(result));
}

void OverlaySession::complete_scroll(HWND source) {
    if (selection_.width() < 64 || selection_.height() < 64) {
        enter_modal();
        MessageBoxW(source, L"长截图选区至少需要 64 x 64 像素。", kAppName, MB_OK | MB_ICONINFORMATION);
        leave_modal();
        return;
    }
    if (!annotations_.empty()) {
        enter_modal();
        MessageBoxW(source, L"请先撤销标注，再开始长截图。", kAppName, MB_OK | MB_ICONINFORMATION);
        leave_modal();
        return;
    }
    for (const auto& window : windows_) {
        ShowWindow(window->hwnd(), SW_HIDE);
    }
    DwmFlush();
    run_scroll_capture(source);
}

void OverlaySession::run_scroll_capture(HWND source) {
    const auto restore_overlay =
        [this, source](std::wstring_view message) {
            for (const auto& window : windows_) {
                if (window && window->hwnd() && IsWindow(window->hwnd())) {
                    ShowWindow(window->hwnd(), SW_SHOWNOACTIVATE);
                    window->invalidate();
                }
            }
            DwmFlush();
            show_output_error(source, message);
        };

    Bitmap first_frame = capture_rect(selection_);
    if (first_frame.empty()) {
        restore_overlay(
            L"长截图初始化失败。已返回当前选区，您可以调整后重试。");
        return;
    }

    auto active = std::make_unique<ActiveScrollCapture>();
    active->last_frame = first_frame;
    active->stitcher = std::make_unique<ScrollStitcher>(std::move(first_frame));
    if (!active->stitcher->valid()) {
        restore_overlay(
            L"长截图选区过大或初始化失败。已返回当前选区，您可以缩小选区后重试。");
        return;
    }
    const POINT selection_center{
        selection_.left + selection_.width() / 2,
        selection_.top + selection_.height() / 2,
    };
    HWND target = WindowFromPoint(selection_center);
    target = target ? GetAncestor(target, GA_ROOT) : nullptr;
    DWORD target_process_id = 0;
    if (target) {
        GetWindowThreadProcessId(target, &target_process_id);
        if (target_process_id == GetCurrentProcessId() ||
            target == GetDesktopWindow() ||
            !IsWindowVisible(target) ||
            IsIconic(target)) {
            target = nullptr;
            target_process_id = 0;
        }
    }
    active->target_window = target;
    active->target_process_id = target_process_id;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    active->border_window = create_scroll_border_window(instance, source, selection_);
    active->control_state.on_tick = [this] { capture_scroll_frame(); };
    active->control_state.on_finish = [this] { finish_scroll_capture(false); };
    active->control_state.on_cancel = [this] { finish_scroll_capture(true); };
    active->control_window =
        create_scroll_control_window(instance, source, selection_, &active->control_state);
    if (!active->control_window) {
        if (active->border_window) {
            DestroyWindow(active->border_window);
        }
        restore_overlay(
            L"无法创建长截图控制窗口。已返回当前选区，您可以重试。");
        return;
    }
    std::wstring progress = std::format(L"{} px", active->stitcher->height());
    SetWindowTextW(active->control_window, progress.c_str());
    scroll_capture_ = std::move(active);
}

void OverlaySession::capture_scroll_frame() {
    if (!scroll_capture_ || scroll_capture_->processing || scroll_capture_->paused || done_) {
        return;
    }

    ActiveScrollCapture& active = *scroll_capture_;
    active.processing = true;
    if (active.frame_worker.joinable()) {
        active.frame_worker.join();
    }
    {
        std::scoped_lock lock(active.frame_mutex);
        active.frame_completion.reset();
    }

    ActiveScrollCapture* const active_ptr = &active;
    const RectI selection = selection_;
    const int locked_direction = active.locked_direction;
    const HWND notification_window = modal_owner();
    try {
        active.frame_worker = std::jthread(
            [active_ptr,
             selection,
             locked_direction,
             notification_window](std::stop_token stop_token) {
                ScrollFrameCompletion completion;
                try {
                    if (stop_token.stop_requested()) {
                        completion.status = ScrollFrameStatus::cancelled;
                    } else {
                        bool target_matches = true;
                        if (active_ptr->target_window) {
                            DWORD process_id = 0;
                            GetWindowThreadProcessId(
                                active_ptr->target_window,
                                &process_id);
                            const HWND foreground = GetForegroundWindow();
                            const HWND foreground_root =
                                foreground
                                    ? GetAncestor(foreground, GA_ROOT)
                                    : nullptr;
                            target_matches =
                                IsWindow(active_ptr->target_window) &&
                                IsWindowVisible(active_ptr->target_window) &&
                                !IsIconic(active_ptr->target_window) &&
                                process_id ==
                                    active_ptr->target_process_id &&
                                foreground_root ==
                                    active_ptr->target_window;
                        }
                        if (!target_matches) {
                            completion.status =
                                ScrollFrameStatus::target_changed;
                        } else {
                            Bitmap new_frame = capture_rect(selection);
                            if (new_frame.empty()) {
                                completion.status =
                                    ScrollFrameStatus::capture_failed;
                            } else if (stop_token.stop_requested()) {
                                completion.status =
                                    ScrollFrameStatus::cancelled;
                            } else {
                                const ScrollResult detected =
                                    detect_scroll(
                                        active_ptr->last_frame,
                                        new_frame,
                                        locked_direction);
                                completion.direction =
                                    detected.direction;
                                if (detected.status() ==
                                    ScrollResult::Status::unchanged) {
                                    completion.status =
                                        ScrollFrameStatus::unchanged;
                                } else if (
                                    detected.status() ==
                                    ScrollResult::Status::mismatch) {
                                    completion.status =
                                        ScrollFrameStatus::mismatch;
                                } else if (
                                    locked_direction != 0 &&
                                    detected.direction !=
                                        locked_direction) {
                                    completion.status =
                                        ScrollFrameStatus::
                                            reverse_direction;
                                } else {
                                    const StitchStatus status =
                                        detected.direction > 0
                                            ? active_ptr->stitcher->append(
                                                  new_frame,
                                                  detected.offset)
                                            : active_ptr->stitcher->prepend(
                                                  new_frame,
                                                  detected.offset);
                                    if (status ==
                                        StitchStatus::success) {
                                        active_ptr->last_frame =
                                            std::move(new_frame);
                                        completion.status =
                                            ScrollFrameStatus::stitched;
                                    } else if (
                                        status ==
                                        StitchStatus::limit_reached) {
                                        completion.status =
                                            ScrollFrameStatus::
                                                stitch_limit;
                                    } else if (
                                        status ==
                                        StitchStatus::
                                            allocation_failed) {
                                        completion.status =
                                            ScrollFrameStatus::
                                                stitch_allocation_failed;
                                    } else {
                                        completion.status =
                                            ScrollFrameStatus::
                                                stitch_failed;
                                    }
                                }
                            }
                        }
                    }
                    completion.stitched_height =
                        active_ptr->stitcher->height();
                } catch (...) {
                    completion.status =
                        ScrollFrameStatus::
                            stitch_allocation_failed;
                }
                {
                    std::scoped_lock lock(active_ptr->frame_mutex);
                    active_ptr->frame_completion =
                        std::move(completion);
                }
                if (notification_window &&
                    IsWindow(notification_window)) {
                    PostMessageW(
                        notification_window,
                        kOverlayScrollFrameCompletedMessage,
                        0,
                        0);
                }
            });
    } catch (...) {
        active.processing = false;
        active.paused = true;
        active.pause_text = L"后台采集失败";
        if (active.control_window &&
            IsWindow(active.control_window)) {
            KillTimer(
                active.control_window,
                kScrollCaptureTimer);
            SetWindowTextW(
                active.control_window,
                active.pause_text.c_str());
        }
    }
}

void OverlaySession::handle_scroll_frame_completion() {
    if (!scroll_capture_) {
        return;
    }
    ActiveScrollCapture& active = *scroll_capture_;
    std::optional<ScrollFrameCompletion> completion;
    {
        std::scoped_lock lock(active.frame_mutex);
        if (!active.frame_completion) {
            return;
        }
        completion = std::move(active.frame_completion);
        active.frame_completion.reset();
    }
    if (active.frame_worker.joinable()) {
        active.frame_worker.join();
    }
    active.processing = false;
    if (done_ ||
        completion->status == ScrollFrameStatus::cancelled) {
        return;
    }

    switch (completion->status) {
        case ScrollFrameStatus::unchanged:
            active.consecutive_failures = 0;
            break;
        case ScrollFrameStatus::capture_failed:
        case ScrollFrameStatus::mismatch:
            ++active.consecutive_failures;
            break;
        case ScrollFrameStatus::target_changed:
            active.paused = true;
            active.pause_text = L"目标已切换";
            break;
        case ScrollFrameStatus::reverse_direction:
            active.paused = true;
            active.pause_text = L"反向暂停";
            break;
        case ScrollFrameStatus::stitched:
            if (active.locked_direction == 0) {
                active.locked_direction = completion->direction;
            }
            active.consecutive_failures = 0;
            break;
        case ScrollFrameStatus::stitch_limit:
            active.paused = true;
            active.pause_text = L"已达上限";
            break;
        case ScrollFrameStatus::stitch_allocation_failed:
            active.paused = true;
            active.pause_text = L"内存不足";
            break;
        case ScrollFrameStatus::stitch_failed:
            active.paused = true;
            active.pause_text = L"拼接已暂停";
            break;
        case ScrollFrameStatus::cancelled:
            break;
    }
    if (!active.paused &&
        active.consecutive_failures >= 4) {
        active.paused = true;
        active.pause_text = L"匹配暂停";
    }

    const HWND control = active.control_window;
    if (control && IsWindow(control)) {
        if (active.paused) {
            KillTimer(control, kScrollCaptureTimer);
        }
        const std::wstring progress =
            active.paused
                ? active.pause_text
                : std::format(
                      L"{} px",
                      completion->stitched_height);
        SetWindowTextW(control, progress.c_str());
    }
}

void OverlaySession::stop_scroll_worker() {
    if (!scroll_capture_) {
        return;
    }
    ActiveScrollCapture& active = *scroll_capture_;
    if (active.frame_worker.joinable()) {
        active.frame_worker.request_stop();
        active.frame_worker.join();
    }
    {
        std::scoped_lock lock(active.frame_mutex);
        active.frame_completion.reset();
    }
    active.processing = false;
}

void OverlaySession::finish_scroll_capture(bool cancelled) {
    if (!scroll_capture_ || done_) {
        return;
    }
    if (scroll_capture_->control_window && IsWindow(scroll_capture_->control_window)) {
        KillTimer(scroll_capture_->control_window, kScrollCaptureTimer);
    }
    stop_scroll_worker();
    if (cancelled) {
        destroy_scroll_windows();
        finish({ExitCode::user_cancelled, L"长截图已取消。"});
        return;
    }

    const auto rearm_output = [this](std::wstring_view status) {
        if (!scroll_capture_) {
            return;
        }
        scroll_capture_->paused = true;
        scroll_capture_->pause_text = status;
        scroll_capture_->control_state.finished = false;
        scroll_capture_->control_state.cancelled = false;
        if (scroll_capture_->control_window &&
            IsWindow(scroll_capture_->control_window)) {
            SetWindowTextW(
                scroll_capture_->control_window,
                scroll_capture_->pause_text.c_str());
        }
    };

    Bitmap stitched;
    const StitchStatus materialized = scroll_capture_->stitcher->materialize(stitched);
    if (materialized != StitchStatus::success || stitched.empty()) {
        rearm_output(L"合并失败");
        show_output_error(
            scroll_capture_->control_window,
            L"长截图合并失败。当前拼接内容仍会保留，您可以重试或取消。");
        return;
    }
    std::wstring error;
    const bool save_to_file =
        request_.action == RegionAction::file ||
        _wcsicmp(request_.config.default_output.c_str(), L"file") == 0;
    if (save_to_file) {
        HWND owner = scroll_capture_->control_window;
        std::optional<std::filesystem::path> path;
        if (request_.action == RegionAction::file && !request_.path.empty()) {
            path = resolve_output_path(request_.path);
        } else {
            enter_modal();
            path = prompt_png_path(owner);
            if (done_) {
                leave_modal();
                return;
            }
            leave_modal();
        }
        if (!path) {
            rearm_output(L"等待保存");
            return;
        }
        if (!save_png(stitched, *path, &error)) {
            rearm_output(L"保存失败");
            show_output_error(
                owner,
                (error.empty() ? L"长截图保存失败。" : error) +
                    std::wstring(
                        L"\n\n当前拼接内容仍会保留，再次点击“完成”可重试。"));
            return;
        }
        RegionResult result{ExitCode::success, L"长截图已保存。"};
        result.path = path->wstring();
        result.bitmap = std::move(stitched);
        destroy_scroll_windows();
        finish(std::move(result));
    } else {
        if (!copy_bitmap_to_clipboard(stitched, &error)) {
            const HWND owner = scroll_capture_->control_window;
            rearm_output(L"复制失败");
            show_output_error(
                owner,
                (error.empty() ? L"长截图复制失败。" : error) +
                    std::wstring(
                        L"\n\n当前拼接内容仍会保留，再次点击“完成”可重试。"));
            return;
        }
        RegionResult result{ExitCode::success, L"长截图已复制到剪贴板。"};
        result.bitmap = std::move(stitched);
        destroy_scroll_windows();
        finish(std::move(result));
    }
}

void OverlaySession::destroy_scroll_windows() {
    if (!scroll_capture_) {
        return;
    }
    stop_scroll_worker();
    const HWND border = scroll_capture_->border_window;
    const HWND control = scroll_capture_->control_window;
    scroll_capture_->border_window = nullptr;
    scroll_capture_->control_window = nullptr;
    scroll_capture_->control_state.on_tick = {};
    scroll_capture_->control_state.on_finish = {};
    scroll_capture_->control_state.on_cancel = {};
    if (control && IsWindow(control)) {
        KillTimer(control, kScrollBlinkTimer);
        KillTimer(control, kScrollCaptureTimer);
        DestroyWindow(control);
    }
    if (border && IsWindow(border)) DestroyWindow(border);
    scroll_capture_.reset();
}

HWND OverlaySession::modal_owner(HWND preferred) const noexcept {
    if (preferred && IsWindow(preferred)) {
        return preferred;
    }
    for (const auto& window : windows_) {
        if (window && window->hwnd() && IsWindow(window->hwnd())) {
            return window->hwnd();
        }
    }
    return nullptr;
}

void OverlaySession::show_output_error(
    HWND owner,
    std::wstring_view message) {
    if (done_) {
        return;
    }
    enter_modal();
    MessageBoxW(
        modal_owner(owner),
        std::wstring(message).c_str(),
        kAppName,
        MB_OK | MB_ICONERROR);
    leave_modal();
    if (!done_) {
        invalidate_all();
    }
}

void OverlaySession::enter_modal() noexcept {
    ++modal_depth_;
}

void OverlaySession::leave_modal() {
    if (modal_depth_ == 0) {
        return;
    }
    --modal_depth_;
    if (modal_depth_ == 0 && completion_pending_) {
        completion_pending_ = false;
        deliver_completion();
    }
}

void OverlaySession::deliver_completion() {
    if (completion_) {
        auto completion = std::move(completion_);
        completion(std::move(result_));
    }
}

void OverlaySession::finish(RegionResult result) {
    if (done_) {
        return;
    }
    stop_ocr_worker();
    result_ = std::move(result);
    if (result_.bounds.empty()) {
        result_.bounds = selection_;
    }
    if (result_.action == RegionAction::interactive) {
        result_.action = request_.action;
    }
    result_.config = request_.config;
    done_ = true;
    if (prompt_window_ && IsWindow(prompt_window_)) {
        DestroyWindow(prompt_window_);
    }
    prompt_window_ = nullptr;
    destroy_scroll_windows();
    for (const auto& window : windows_) {
        ShowWindow(window->hwnd(), SW_HIDE);
    }
    if (modal_depth_ != 0) {
        completion_pending_ = true;
    } else {
        deliver_completion();
    }
}

}  // namespace airshot::overlay_detail
