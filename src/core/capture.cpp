#include "airshot/capture.h"

#include "capture_modern.h"
#include "capture_uia.h"

#include <dwmapi.h>

#include <array>
#include <cstring>
#include <cwctype>
#include <limits>
#include <tuple>

namespace airshot {
namespace {

struct CheckedRect {
    RectI rect;
    int width{};
    int height{};
};

[[nodiscard]] std::optional<CheckedRect> normalize_checked(const RectI& rect) noexcept {
    const std::int64_t left = std::min<std::int64_t>(rect.left, rect.right);
    const std::int64_t top = std::min<std::int64_t>(rect.top, rect.bottom);
    const std::int64_t right = std::max<std::int64_t>(rect.left, rect.right);
    const std::int64_t bottom = std::max<std::int64_t>(rect.top, rect.bottom);
    const std::int64_t width = right - left;
    const std::int64_t height = bottom - top;
    if (width <= 0 || height <= 0 ||
        width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return CheckedRect{
        RectI{
            static_cast<int>(left),
            static_cast<int>(top),
            static_cast<int>(right),
            static_cast<int>(bottom),
        },
        static_cast<int>(width),
        static_cast<int>(height),
    };
}

[[nodiscard]] std::optional<RectI> intersect_checked(const RectI& first, const RectI& second) noexcept {
    const RectI value{
        std::max(first.left, second.left),
        std::max(first.top, second.top),
        std::min(first.right, second.right),
        std::min(first.bottom, second.bottom),
    };
    if (value.left >= value.right || value.top >= value.bottom) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<int> checked_difference(int first, int second) noexcept {
    const std::int64_t value = static_cast<std::int64_t>(first) - second;
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

[[nodiscard]] bool bitmap_matches_rect(
    const Bitmap& bitmap,
    const RectI& rect) noexcept {
    return bitmap.valid() &&
           bitmap.width == rect.width() &&
           bitmap.height == rect.height();
}

class ScreenDc {
public:
    explicit ScreenDc(HWND window) noexcept : window_(window), value_(GetDC(window)) {}
    ~ScreenDc() {
        if (value_) {
            ReleaseDC(window_, value_);
        }
    }
    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;

    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HWND window_{};
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

class OwnedBitmap {
public:
    explicit OwnedBitmap(HBITMAP value = nullptr) noexcept : value_(value) {}
    ~OwnedBitmap() {
        if (value_) {
            DeleteObject(value_);
        }
    }
    OwnedBitmap(const OwnedBitmap&) = delete;
    OwnedBitmap& operator=(const OwnedBitmap&) = delete;

    [[nodiscard]] HBITMAP get() const noexcept { return value_; }

private:
    HBITMAP value_{};
};

class OwnedIcon {
public:
    explicit OwnedIcon(HICON value = nullptr) noexcept : value_(value) {}
    ~OwnedIcon() {
        if (value_) {
            DestroyIcon(value_);
        }
    }
    OwnedIcon(const OwnedIcon&) = delete;
    OwnedIcon& operator=(const OwnedIcon&) = delete;
    OwnedIcon(OwnedIcon&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    OwnedIcon& operator=(OwnedIcon&& other) noexcept {
        if (this != &other) {
            if (value_) {
                DestroyIcon(value_);
            }
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] HICON get() const noexcept { return value_; }

private:
    HICON value_{};
};

struct CursorSnapshot {
    OwnedIcon icon;
    POINT top_left{};
    SIZE size{};
};

class SelectedObject {
public:
    SelectedObject(HDC dc, HGDIOBJ object) noexcept : dc_(dc), previous_(SelectObject(dc, object)) {}
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

[[nodiscard]] Bitmap capture_with_gdi(const RectI& requested_rect) {
    const auto checked = normalize_checked(requested_rect);
    if (!checked) {
        return {};
    }

    Bitmap result(checked->width, checked->height);
    if (result.empty()) {
        return {};
    }

    ScreenDc screen_dc(nullptr);
    if (!screen_dc.get()) {
        return {};
    }
    MemoryDc memory_dc(screen_dc.get());
    if (!memory_dc.get()) {
        return {};
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = result.width;
    info.bmiHeader.biHeight = -result.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(result.pixels.size());

    void* bits = nullptr;
    OwnedBitmap dib(CreateDIBSection(screen_dc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!dib.get() || !bits) {
        return {};
    }
    SelectedObject selection(memory_dc.get(), dib.get());
    if (!selection.valid()) {
        return {};
    }

    if (!BitBlt(memory_dc.get(),
                0,
                0,
                result.width,
                result.height,
                screen_dc.get(),
                checked->rect.left,
                checked->rect.top,
                SRCCOPY | CAPTUREBLT) ||
        !GdiFlush()) {
        return {};
    }

    std::memcpy(result.pixels.data(), bits, result.pixels.size());
    result.make_opaque();
    return result;
}

[[nodiscard]] std::optional<CursorSnapshot>
snapshot_visible_cursor() noexcept {
    CURSORINFO cursor_info{sizeof(cursor_info)};
    if (!GetCursorInfo(&cursor_info) ||
        (cursor_info.flags & CURSOR_SHOWING) == 0 ||
        !cursor_info.hCursor) {
        return std::nullopt;
    }

    OwnedIcon icon(CopyIcon(cursor_info.hCursor));
    if (!icon.get()) {
        return std::nullopt;
    }

    ICONINFO icon_info{};
    if (!GetIconInfo(icon.get(), &icon_info)) {
        return std::nullopt;
    }

    int width = std::max(1, GetSystemMetrics(SM_CXCURSOR));
    int height = std::max(1, GetSystemMetrics(SM_CYCURSOR));
    BITMAP metrics{};
    const HBITMAP metrics_bitmap =
        icon_info.hbmColor ? icon_info.hbmColor : icon_info.hbmMask;
    if (metrics_bitmap &&
        GetObjectW(
            metrics_bitmap,
            static_cast<int>(sizeof(metrics)),
            &metrics) ==
            static_cast<int>(sizeof(metrics))) {
        width = std::max(1L, metrics.bmWidth);
        height = std::max(
            1L,
            icon_info.hbmColor ? metrics.bmHeight
                               : metrics.bmHeight / 2L);
    }
    if (icon_info.hbmColor) {
        DeleteObject(icon_info.hbmColor);
    }
    if (icon_info.hbmMask) {
        DeleteObject(icon_info.hbmMask);
    }

    const std::int64_t left =
        static_cast<std::int64_t>(cursor_info.ptScreenPos.x) -
        static_cast<std::int64_t>(icon_info.xHotspot);
    const std::int64_t top =
        static_cast<std::int64_t>(cursor_info.ptScreenPos.y) -
        static_cast<std::int64_t>(icon_info.yHotspot);
    if (left < std::numeric_limits<int>::min() ||
        left > std::numeric_limits<int>::max() ||
        top < std::numeric_limits<int>::min() ||
        top > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return CursorSnapshot{
        std::move(icon),
        {static_cast<int>(left), static_cast<int>(top)},
        {width, height},
    };
}

void composite_cursor(
    Bitmap& bitmap,
    const RectI& capture_bounds,
    const CursorSnapshot& cursor) noexcept {
    if (!bitmap_matches_rect(bitmap, capture_bounds) ||
        !cursor.icon.get() || cursor.size.cx <= 0 || cursor.size.cy <= 0) {
        return;
    }
    const std::int64_t cursor_right =
        static_cast<std::int64_t>(cursor.top_left.x) + cursor.size.cx;
    const std::int64_t cursor_bottom =
        static_cast<std::int64_t>(cursor.top_left.y) + cursor.size.cy;
    if (cursor_right <= capture_bounds.left ||
        cursor_bottom <= capture_bounds.top ||
        cursor.top_left.x >= capture_bounds.right ||
        cursor.top_left.y >= capture_bounds.bottom) {
        return;
    }

    ScreenDc screen_dc(nullptr);
    if (!screen_dc.get()) {
        return;
    }
    MemoryDc memory_dc(screen_dc.get());
    if (!memory_dc.get()) {
        return;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap.width;
    info.bmiHeader.biHeight = -bitmap.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage =
        static_cast<DWORD>(bitmap.pixels.size());

    void* bits = nullptr;
    OwnedBitmap dib(CreateDIBSection(
        screen_dc.get(),
        &info,
        DIB_RGB_COLORS,
        &bits,
        nullptr,
        0));
    if (!dib.get() || !bits) {
        return;
    }
    SelectedObject selection(memory_dc.get(), dib.get());
    if (!selection.valid()) {
        return;
    }
    std::memcpy(bits, bitmap.pixels.data(), bitmap.pixels.size());

    const int x = cursor.top_left.x - capture_bounds.left;
    const int y = cursor.top_left.y - capture_bounds.top;
    if (!DrawIconEx(
            memory_dc.get(),
            x,
            y,
            cursor.icon.get(),
            cursor.size.cx,
            cursor.size.cy,
            0,
            nullptr,
            DI_NORMAL) ||
        !GdiFlush()) {
        return;
    }
    std::memcpy(bitmap.pixels.data(), bits, bitmap.pixels.size());
    bitmap.make_opaque();
}

[[nodiscard]] std::optional<RectI> virtual_desktop_bounds() noexcept {
    const std::int64_t left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const std::int64_t top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const std::int64_t width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const std::int64_t height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const std::int64_t right = left + width;
    const std::int64_t bottom = top + height;
    if (width <= 0 || height <= 0 ||
        right < std::numeric_limits<int>::min() || right > std::numeric_limits<int>::max() ||
        bottom < std::numeric_limits<int>::min() || bottom > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return RectI{
        static_cast<int>(left),
        static_cast<int>(top),
        static_cast<int>(right),
        static_cast<int>(bottom),
    };
}

[[nodiscard]] std::optional<RectI> window_bounds(HWND window) {
    if (!window) {
        return std::nullopt;
    }
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))) &&
        !GetWindowRect(window, &rect)) {
        return std::nullopt;
    }
    const RectI result = RectI::from_native(rect);
    return normalize_checked(result) ? std::optional(result) : std::nullopt;
}

BOOL CALLBACK collect_monitors(HMONITOR monitor, HDC, LPRECT rect, LPARAM context) {
    auto* monitors = reinterpret_cast<std::vector<MonitorSnapshot>*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) {
        return TRUE;
    }

    MonitorSnapshot snapshot;
    snapshot.handle = monitor;
    snapshot.bounds = RectI::from_native(*rect);
    snapshot.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    snapshot.device_name = info.szDevice;
    monitors->push_back(std::move(snapshot));
    return TRUE;
}

[[nodiscard]] std::vector<MonitorSnapshot> enumerate_monitors() {
    std::vector<MonitorSnapshot> result;
    if (!EnumDisplayMonitors(nullptr, nullptr, collect_monitors, reinterpret_cast<LPARAM>(&result))) {
        return {};
    }
    std::ranges::sort(result, [](const MonitorSnapshot& first, const MonitorSnapshot& second) {
        return std::tie(first.bounds.left, first.bounds.top, first.device_name) <
               std::tie(second.bounds.left, second.bounds.top, second.device_name);
    });
    return result;
}

BOOL CALLBACK collect_windows(HWND window, LPARAM context) {
    auto* windows =
        reinterpret_cast<std::vector<WindowCandidate>*>(context);
    if (!windows || windows->size() >= 8192) {
        return FALSE;
    }
    // A meaningful number of modern/custom windows intentionally have no
    // title. Geometry and compositor visibility are the reliable eligibility
    // checks; requiring caption text makes smart selection skip those apps.
    wchar_t class_name[128]{};
    const int class_name_length = GetClassNameW(
        window,
        class_name,
        static_cast<int>(std::size(class_name)));
    constexpr wchar_t airshot_class_prefix[] = L"AirScreenshot.";
    const bool is_airshot_window =
        class_name_length >=
            static_cast<int>(std::size(airshot_class_prefix) - 1) &&
        _wcsnicmp(
            class_name,
            airshot_class_prefix,
            std::size(airshot_class_prefix) - 1) == 0;
    if (!IsWindowVisible(window) || IsIconic(window) ||
        is_airshot_window) {
        return TRUE;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return TRUE;
    }
    const auto bounds = window_bounds(window);
    if (!bounds) {
        return TRUE;
    }
    const auto checked = normalize_checked(*bounds);
    if (!checked || checked->width <= 6 || checked->height <= 6) {
        return TRUE;
    }
    windows->push_back({window, *bounds, window, nullptr, 0});

    struct ChildCollection {
        std::vector<WindowCandidate>* candidates{};
        HWND root{};
        RectI root_bounds;
        std::size_t root_index{};
    } children{windows, window, *bounds, windows->size() - 1};
    EnumChildWindows(
        window,
        [](HWND child, LPARAM child_context) -> BOOL {
            auto* collection =
                reinterpret_cast<ChildCollection*>(child_context);
            if (!collection ||
                collection->candidates->size() >= 8192 ||
                !IsWindowVisible(child)) {
                return collection &&
                               collection->candidates->size() >= 8192
                           ? FALSE
                           : TRUE;
            }

            RECT native_bounds{};
            if (!GetWindowRect(child, &native_bounds)) {
                return TRUE;
            }
            const RectI child_bounds =
                RectI::from_native(native_bounds).normalized();
            const auto clipped = intersect(
                child_bounds,
                collection->root_bounds);
            if (!clipped ||
                clipped->width() <= 6 ||
                clipped->height() <= 6 ||
                (clipped->left == collection->root_bounds.left &&
                 clipped->top == collection->root_bounds.top &&
                 clipped->right == collection->root_bounds.right &&
                 clipped->bottom == collection->root_bounds.bottom)) {
                return TRUE;
            }
            const bool duplicate_bounds = std::ranges::any_of(
                collection->candidates->begin() +
                    static_cast<std::ptrdiff_t>(collection->root_index),
                collection->candidates->end(),
                [&](const WindowCandidate& candidate) {
                    return candidate.bounds.left == clipped->left &&
                           candidate.bounds.top == clipped->top &&
                           candidate.bounds.right == clipped->right &&
                           candidate.bounds.bottom == clipped->bottom;
                });
            if (duplicate_bounds) {
                return TRUE;
            }

            int depth = 1;
            for (HWND parent = GetParent(child);
                 parent && parent != collection->root && depth < 32;
                 parent = GetParent(parent)) {
                ++depth;
            }
            collection->candidates->push_back(
                {child, *clipped, collection->root, nullptr, depth});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&children));

    // Resolve every retained control to its closest retained ancestor. Some
    // framework wrappers share identical bounds and are intentionally omitted,
    // so the immediate Win32 parent is not necessarily a selectable candidate.
    const std::size_t group_end = windows->size();
    for (std::size_t index = children.root_index + 1;
         index < group_end;
         ++index) {
        HWND parent = GetParent((*windows)[index].handle);
        while (parent && parent != window) {
            const auto retained = std::find_if(
                windows->begin() +
                    static_cast<std::ptrdiff_t>(children.root_index + 1),
                windows->begin() + static_cast<std::ptrdiff_t>(group_end),
                [parent](const WindowCandidate& candidate) {
                    return candidate.handle == parent;
                });
            if (retained !=
                windows->begin() + static_cast<std::ptrdiff_t>(group_end)) {
                break;
            }
            parent = GetParent(parent);
        }
        (*windows)[index].parent =
            parent && parent != (*windows)[index].handle ? parent : window;
    }
    return TRUE;
}

[[nodiscard]] Bitmap capture_from_monitors(const std::vector<MonitorSnapshot>& monitors,
                                           const RectI& selection) {
    const auto checked = normalize_checked(selection);
    if (!checked) {
        return {};
    }
    Bitmap result(checked->width, checked->height);
    if (result.empty()) {
        return {};
    }

    for (const auto& monitor : monitors) {
        const auto overlap = intersect_checked(monitor.bounds, checked->rect);
        if (!overlap) {
            continue;
        }
        Bitmap captured = capture_with_gdi(*overlap);
        if (captured.empty()) {
            return {};
        }
        const auto target_x = checked_difference(overlap->left, checked->rect.left);
        const auto target_y = checked_difference(overlap->top, checked->rect.top);
        if (!target_x || !target_y) {
            return {};
        }
        const RectI source_rect{0, 0, captured.width, captured.height};
        const POINT target_origin{*target_x, *target_y};
        blit(captured, source_rect, result, target_origin);
    }
    return result;
}

}  // namespace

std::vector<MonitorSnapshot> capture_monitors(bool include_cursor) {
    std::optional<CursorSnapshot> cursor;
    if (include_cursor) {
        cursor = snapshot_visible_cursor();
    }
    auto result = enumerate_monitors();
    for (auto& monitor : result) {
        monitor.bitmap =
            capture_detail::capture_monitor_modern(monitor.handle);
        if (!bitmap_matches_rect(
                monitor.bitmap,
                monitor.bounds)) {
            monitor.bitmap = capture_with_gdi(monitor.bounds);
        }
        if (cursor) {
            composite_cursor(monitor.bitmap, monitor.bounds, *cursor);
        }
    }
    return result;
}

std::vector<DisplayMonitorGeometry> current_display_topology() {
    const auto monitors = enumerate_monitors();
    std::vector<DisplayMonitorGeometry> result;
    result.reserve(monitors.size());
    for (const auto& monitor : monitors) {
        result.push_back({
            monitor.bounds,
            monitor.primary,
            monitor.device_name});
    }
    return result;
}

std::vector<WindowCandidate> enumerate_window_candidates() {
    capture_uia::begin_candidate_session();
    std::vector<WindowCandidate> result;
    if (!EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&result))) {
        return {};
    }
    return result;
}

std::optional<WindowCandidate> window_candidate_at_point(
    std::span<const WindowCandidate> candidates,
    POINT point,
    std::size_t ancestor_offset) noexcept {
    for (std::size_t root_index = 0;
         root_index < candidates.size();) {
        const WindowCandidate& top_level = candidates[root_index];
        const HWND root = top_level.root ? top_level.root : top_level.handle;
        std::size_t group_end = root_index + 1;
        while (group_end < candidates.size()) {
            const HWND candidate_root = candidates[group_end].root
                                            ? candidates[group_end].root
                                            : candidates[group_end].handle;
            if (candidate_root != root) {
                break;
            }
            ++group_end;
        }

        if (!top_level.bounds.contains(point)) {
            root_index = group_end;
            continue;
        }

        std::size_t best_index = std::numeric_limits<std::size_t>::max();
        int best_depth = -1;
        std::uint64_t best_area = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t index = root_index;
             index < group_end;
             ++index) {
            const WindowCandidate& candidate = candidates[index];
            if (!candidate.bounds.contains(point)) {
                continue;
            }
            const int depth = std::clamp(candidate.depth, 0, 32);
            const std::uint64_t area =
                static_cast<std::uint64_t>(candidate.bounds.width()) *
                static_cast<std::uint64_t>(candidate.bounds.height());
            if (depth > best_depth ||
                (depth == best_depth && area < best_area)) {
                best_depth = depth;
                best_area = area;
                best_index = index;
            }
        }
        if (best_index == std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }

        std::array<std::size_t, 33> hwnd_chain{};
        std::size_t hwnd_chain_count = 1;
        hwnd_chain[0] = best_index;
        std::size_t selected = best_index;
        while (hwnd_chain_count < hwnd_chain.size()) {
            const HWND parent = candidates[selected].parent;
            if (!parent || parent == candidates[selected].handle) {
                break;
            }
            std::size_t parent_index = std::numeric_limits<std::size_t>::max();
            for (std::size_t index = root_index;
                 index < group_end;
                 ++index) {
                if (candidates[index].handle == parent) {
                    parent_index = index;
                    break;
                }
            }
            if (parent_index == std::numeric_limits<std::size_t>::max() ||
                parent_index == selected) {
                selected = root_index;
            } else {
                selected = parent_index;
            }
            hwnd_chain[hwnd_chain_count++] = selected;
            if (selected == root_index) {
                break;
            }
        }

        const capture_uia::CandidateChain uia_chain =
            capture_uia::cached_chain_and_request(
                point,
                root,
                top_level.bounds);
        struct MergedCandidate {
            RectI bounds;
            std::size_t hwnd_index{};
            bool from_uia{};
        };
        std::array<
            MergedCandidate,
            33 + capture_uia::kMaxCandidateCount>
            merged{};
        std::size_t merged_count = 0;
        const auto append_merged = [&](MergedCandidate value) noexcept {
            if (merged_count >= merged.size() ||
                !value.bounds.contains(point)) {
                return;
            }
            for (std::size_t index = 0; index < merged_count; ++index) {
                const RectI& existing = merged[index].bounds;
                if (existing.left == value.bounds.left &&
                    existing.top == value.bounds.top &&
                    existing.right == value.bounds.right &&
                    existing.bottom == value.bounds.bottom) {
                    return;
                }
            }
            merged[merged_count++] = value;
        };

        // Merge two real ancestry chains without reordering either one. This
        // preserves the existing HWND wheel hierarchy while UIA fills only the
        // gaps used by Chromium, Electron, WinUI, and custom-rendered controls.
        const auto contains_rect = [](
                                       const RectI& outer,
                                       const RectI& inner) noexcept {
            return outer.left <= inner.left && outer.top <= inner.top &&
                   outer.right >= inner.right &&
                   outer.bottom >= inner.bottom;
        };
        const auto area = [](const RectI& bounds) noexcept {
            return static_cast<std::uint64_t>(bounds.width()) *
                   static_cast<std::uint64_t>(bounds.height());
        };
        std::size_t hwnd_level = 0;
        std::size_t uia_level = 0;
        while (hwnd_level < hwnd_chain_count ||
               uia_level < uia_chain.count) {
            if (hwnd_level >= hwnd_chain_count) {
                append_merged(MergedCandidate{
                    uia_chain.candidates[uia_level++],
                    root_index,
                    true,
                });
                continue;
            }
            if (uia_level >= uia_chain.count) {
                append_merged(MergedCandidate{
                    candidates[hwnd_chain[hwnd_level]].bounds,
                    hwnd_chain[hwnd_level],
                    false,
                });
                ++hwnd_level;
                continue;
            }

            const RectI& hwnd_bounds =
                candidates[hwnd_chain[hwnd_level]].bounds;
            const RectI& uia_bounds =
                uia_chain.candidates[uia_level];
            const bool same_bounds =
                hwnd_bounds.left == uia_bounds.left &&
                hwnd_bounds.top == uia_bounds.top &&
                hwnd_bounds.right == uia_bounds.right &&
                hwnd_bounds.bottom == uia_bounds.bottom;
            if (same_bounds) {
                append_merged(MergedCandidate{
                    hwnd_bounds,
                    hwnd_chain[hwnd_level],
                    false,
                });
                ++hwnd_level;
                ++uia_level;
                continue;
            }

            const bool uia_is_deeper =
                contains_rect(hwnd_bounds, uia_bounds) ||
                (!contains_rect(uia_bounds, hwnd_bounds) &&
                 area(uia_bounds) < area(hwnd_bounds));
            if (uia_is_deeper) {
                append_merged(MergedCandidate{
                    uia_bounds,
                    root_index,
                    true,
                });
                ++uia_level;
            } else {
                append_merged(MergedCandidate{
                    hwnd_bounds,
                    hwnd_chain[hwnd_level],
                    false,
                });
                ++hwnd_level;
            }
        }
        if (merged_count == 0) {
            return std::nullopt;
        }
        const MergedCandidate& chosen = merged[std::min(
            ancestor_offset,
            merged_count - 1)];
        if (!chosen.from_uia) {
            return candidates[chosen.hwnd_index];
        }
        return WindowCandidate{
            root,
            chosen.bounds,
            root,
            root,
            32,
        };
    }
    return std::nullopt;
}

Bitmap capture_rect(const RectI& rect) {
    return capture_rect(rect, false);
}

Bitmap capture_rect(
    const RectI& rect,
    bool include_cursor) {
    std::optional<CursorSnapshot> cursor;
    if (include_cursor) {
        cursor = snapshot_visible_cursor();
    }
    const auto monitors = enumerate_monitors();
    Bitmap result =
        monitors.empty() ? Bitmap{} : capture_from_monitors(monitors, rect);
    if (cursor && result.valid()) {
        composite_cursor(result, rect.normalized(), *cursor);
    }
    return result;
}

Bitmap capture_virtual_desktop(bool include_cursor) {
    const auto bounds = virtual_desktop_bounds();
    auto monitors = capture_monitors(include_cursor);
    if (!bounds || monitors.empty()) {
        return {};
    }
    return compose_selection(monitors, *bounds);
}

std::optional<std::pair<Bitmap, RectI>> capture_active_window(
    bool include_cursor) {
    std::optional<CursorSnapshot> cursor;
    if (include_cursor) {
        cursor = snapshot_visible_cursor();
    }
    const HWND window = GetForegroundWindow();
    auto bounds = window_bounds(window);
    if (!bounds) {
        return std::nullopt;
    }
    Bitmap bitmap = capture_detail::capture_window_modern(window);
    if (!bitmap.empty() &&
        (bitmap.width != bounds->width() ||
         bitmap.height != bounds->height())) {
        // WGC follows the compositor's capture item size, which can differ
        // from DWM's extended-frame rectangle by invisible resize borders.
        // Keep the returned coordinate space aligned with the pixels.
        bounds->right = bounds->left + bitmap.width;
        bounds->bottom = bounds->top + bitmap.height;
    }
    if (bitmap.empty()) {
        bitmap = capture_rect(*bounds);
    }
    if (bitmap.empty()) {
        return std::nullopt;
    }
    if (cursor) {
        composite_cursor(bitmap, *bounds, *cursor);
    }
    return std::pair{std::move(bitmap), *bounds};
}

std::optional<std::pair<Bitmap, RectI>> capture_monitor(
    std::wstring_view selector,
    bool include_cursor) {
    if (selector.empty() || _wcsicmp(std::wstring(selector).c_str(), L"all") == 0) {
        const auto bounds = virtual_desktop_bounds();
        if (!bounds) {
            return std::nullopt;
        }
        Bitmap bitmap = capture_virtual_desktop(include_cursor);
        return bitmap.empty() ? std::nullopt : std::optional(std::pair{std::move(bitmap), *bounds});
    }

    auto monitors = enumerate_monitors();
    if (monitors.empty()) {
        return std::nullopt;
    }

    std::size_t index = 0;
    const std::wstring value(selector);
    if (_wcsicmp(value.c_str(), L"primary") == 0) {
        const auto found = std::ranges::find_if(monitors, [](const auto& monitor) { return monitor.primary; });
        index = found == monitors.end() ? 0U : static_cast<std::size_t>(std::distance(monitors.begin(), found));
    } else if (_wcsicmp(value.c_str(), L"cursor") == 0) {
        POINT cursor{};
        if (!GetCursorPos(&cursor)) {
            return std::nullopt;
        }
        const HMONITOR target = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        const auto found =
            std::ranges::find_if(monitors, [target](const auto& monitor) { return monitor.handle == target; });
        index = found == monitors.end() ? 0U : static_cast<std::size_t>(std::distance(monitors.begin(), found));
    } else {
        try {
            std::size_t parsed_characters = 0;
            const unsigned long parsed = std::stoul(value, &parsed_characters);
            if (parsed_characters != value.size()) {
                return std::nullopt;
            }
            index = static_cast<std::size_t>(parsed);
        } catch (...) {
            return std::nullopt;
        }
        if (index >= monitors.size()) {
            return std::nullopt;
        }
    }

    std::optional<CursorSnapshot> cursor;
    if (include_cursor) {
        cursor = snapshot_visible_cursor();
    }
    Bitmap bitmap =
        capture_detail::capture_monitor_modern(monitors[index].handle);
    if (!bitmap_matches_rect(
            bitmap,
            monitors[index].bounds)) {
        bitmap = capture_with_gdi(monitors[index].bounds);
    }
    if (cursor) {
        composite_cursor(bitmap, monitors[index].bounds, *cursor);
    }
    return bitmap.empty() ? std::nullopt
                          : std::optional(std::pair{std::move(bitmap), monitors[index].bounds});
}

Bitmap compose_selection(const std::vector<MonitorSnapshot>& monitors, const RectI& selection) {
    const auto checked = normalize_checked(selection);
    if (!checked) {
        return {};
    }
    Bitmap result(checked->width, checked->height);
    if (result.empty()) {
        return {};
    }

    for (const auto& monitor : monitors) {
        const auto overlap = intersect_checked(monitor.bounds, checked->rect);
        if (!overlap || monitor.bitmap.empty()) {
            continue;
        }
        const auto source_left = checked_difference(overlap->left, monitor.bounds.left);
        const auto source_top = checked_difference(overlap->top, monitor.bounds.top);
        const auto source_right = checked_difference(overlap->right, monitor.bounds.left);
        const auto source_bottom = checked_difference(overlap->bottom, monitor.bounds.top);
        const auto target_x = checked_difference(overlap->left, checked->rect.left);
        const auto target_y = checked_difference(overlap->top, checked->rect.top);
        if (!source_left || !source_top || !source_right || !source_bottom || !target_x || !target_y) {
            continue;
        }
        const RectI source_rect{
            *source_left,
            *source_top,
            *source_right,
            *source_bottom,
        };
        const POINT target_origin{*target_x, *target_y};
        blit(monitor.bitmap, source_rect, result, target_origin);
    }
    return result;
}

}  // namespace airshot
