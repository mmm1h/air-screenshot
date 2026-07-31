#include "settings_window.h"

#include "app_icon.h"

#include "airshot/ocr.h"
#include "airshot/strings.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>
#include <string>
#include <format>
#include <cwctype>
#include <iterator>
#include <windowsx.h>
#include <thread>
#include <filesystem>
#include <dwmapi.h>
#include <new>

/*
THESIS: Make the real screenshot path visible and controllable; refuse the card-wall settings page.
OWN-WORLD: A near-black precision rail, cool neutral canvas, open setting rows, cobalt action states,
and cyan only for healthy workflow status.
STORY: Users choose a capture destination, tune annotation and OCR, arrange tools, record shortcuts,
and understand what Save will change.
FIRST VIEWPORT: Five destinations live in a 224-DIP rail; the selected task opens beside a compact
capture-pipeline strip, with Save state anchored to the bottom action bar.
FORM: Capture Console, the chosen rail-and-open-row hybrid from the Stitch structure study
(project 454227981142275123, seed stitch/450cf4261edd46d287f5f04e5a5a7428).
*/

namespace airshot {
namespace {

constexpr int kSettingsWidth = 920;
constexpr int kSettingsHeight = 720;
constexpr int kWorkAreaMargin = 8;
constexpr int kTitleBarHeight = 48;
constexpr int kRailWidth = 224;
constexpr int kFooterTop = 656;
constexpr int kCategoryCount = 5;
constexpr int kCategoryTop = 96;
constexpr int kCategoryPitch = 48;
constexpr int kSettingRowHeight = 56;
constexpr int kToolbarRowHeight = 40;
constexpr int kToolbarVisibleRows = 11;

enum class SettingsFocusKind {
    category,
    capture_annotation,
    capture_locked_tool,
    capture_output,
    capture_notifications,
    reset_serial,
    ocr_enabled,
    font_family,
    text_bold,
    text_italic,
    ocr_engine,
    download_ocr,
    toolbar_item,
    toolbar_visibility,
    toolbar_move_up,
    toolbar_move_down,
    global_ocr_enabled,
    shortcut,
    app_shell,
    app_startup,
    theme,
    app_icon,
    save,
    cancel,
    close,
};

struct SettingsFocusTarget {
    SettingsFocusKind kind{};
    int index{-1};

    bool operator==(const SettingsFocusTarget&) const = default;
};

enum ShortcutIdx {
    idx_capture_hotkey = 0,
    idx_global_ocr_hotkey,
    idx_capture_ocr_shortcut,
    idx_tool_shortcut_select,
    idx_tool_shortcut_rectangle,
    idx_tool_shortcut_ellipse,
    idx_tool_shortcut_line,
    idx_tool_shortcut_arrow,
    idx_tool_shortcut_pen,
    idx_tool_shortcut_mosaic,
    idx_tool_shortcut_blur,
    idx_tool_shortcut_highlight,
    idx_tool_shortcut_text,
    idx_tool_shortcut_serial,
    idx_tool_shortcut_eraser,
    shortcut_count
};

struct SettingsState {
    AppConfig config;
    bool accepted{};
    HWND window{};
    HWND owner{};
    SettingsWindowCompletion completion;
    bool is_light_theme{};
    bool high_contrast{};

    // UI state
    AppConfig initial_config;
    int active_tab{0}; // 0: 截图与输出, 1: 文本与 OCR, 2: 工具栏, 3: 快捷键, 4: 应用与外观
    int capturing_idx_{-1}; // capturing hotkey index
    int selected_tool_idx{-1};
    int toolbar_scroll_offset{};
    int dragging_tool_idx{-1};
    int drag_target_idx{-1};
    POINT mouse_pos{};
    std::optional<SettingsFocusTarget> keyboard_focus;
    bool keyboard_focus_visible{};
    std::optional<std::wstring> shortcut_error;

    // D2D Resources
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target;

    // Brushes
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bg_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sidebar_bg_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_white_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_grey_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> blue_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover_blue_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent_text_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> active_tab_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> control_bg_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> switch_track_off_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> card_bg_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> card_border_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> separator_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover_bg_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cancel_btn_border_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent_indicator_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> keycap_shadow_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> switch_glow_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> close_btn_hover_bg_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> red_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> rail_text_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> rail_muted_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent_soft_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cyan_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> disabled_bg_brush;

    // Text Formats
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> section_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> small_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> utility_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> rail_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> hotkey_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> btn_text_format;

    // Download state
    bool is_downloading{false};
    int download_progress{0};
    std::wstring download_error;
    OcrDependencyStatus ocr_dependency;
    UINT_PTR download_subscription{};
    unsigned int modal_depth{};
    bool nc_destroyed{};
    bool finalized{};
};

float settings_layout_scale(HWND window) noexcept {
    RECT client{};
    if (!window || !GetClientRect(window, &client)) {
        return 1.0f;
    }
    const float width_scale =
        static_cast<float>(client.right - client.left) / static_cast<float>(kSettingsWidth);
    const float height_scale =
        static_cast<float>(client.bottom - client.top) / static_cast<float>(kSettingsHeight);
    const float scale = std::min(width_scale, height_scale);
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

int settings_scale_dip(HWND window, int value) noexcept {
    return static_cast<int>(std::lround(value * settings_layout_scale(window)));
}

RECT settings_work_area(HMONITOR monitor) noexcept {
    MONITORINFO info{sizeof(info)};
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }
    return {
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
}

SIZE fitted_settings_size(UINT dpi, const RECT& work_area) noexcept {
    const float desired_scale = static_cast<float>(dpi == 0 ? 96 : dpi) / 96.0f;
    const int available_width =
        std::max(1L, work_area.right - work_area.left - 2L * kWorkAreaMargin);
    const int available_height =
        std::max(1L, work_area.bottom - work_area.top - 2L * kWorkAreaMargin);
    const float scale = std::min({
        desired_scale,
        static_cast<float>(available_width) / static_cast<float>(kSettingsWidth),
        static_cast<float>(available_height) / static_cast<float>(kSettingsHeight),
    });
    return {
        std::max(1, static_cast<int>(std::floor(kSettingsWidth * scale))),
        std::max(1, static_cast<int>(std::floor(kSettingsHeight * scale))),
    };
}

std::wstring* get_shortcut_ptr(AppConfig& config, int idx) {
    switch (idx) {
        case idx_capture_hotkey: return &config.capture_hotkey;
        case idx_global_ocr_hotkey: return &config.global_ocr_hotkey;
        case idx_capture_ocr_shortcut: return &config.capture_ocr_shortcut;
        case idx_tool_shortcut_select: return &config.tool_shortcut_select;
        case idx_tool_shortcut_rectangle: return &config.tool_shortcut_rectangle;
        case idx_tool_shortcut_ellipse: return &config.tool_shortcut_ellipse;
        case idx_tool_shortcut_line: return &config.tool_shortcut_line;
        case idx_tool_shortcut_arrow: return &config.tool_shortcut_arrow;
        case idx_tool_shortcut_pen: return &config.tool_shortcut_pen;
        case idx_tool_shortcut_mosaic: return &config.tool_shortcut_mosaic;
        case idx_tool_shortcut_blur: return &config.tool_shortcut_blur;
        case idx_tool_shortcut_highlight: return &config.tool_shortcut_highlight;
        case idx_tool_shortcut_text: return &config.tool_shortcut_text;
        case idx_tool_shortcut_serial: return &config.tool_shortcut_serial;
        case idx_tool_shortcut_eraser: return &config.tool_shortcut_eraser;
        default: return nullptr;
    }
}

const std::wstring* get_shortcut_ptr(const AppConfig& config, int idx) {
    switch (idx) {
        case idx_capture_hotkey: return &config.capture_hotkey;
        case idx_global_ocr_hotkey: return &config.global_ocr_hotkey;
        case idx_capture_ocr_shortcut: return &config.capture_ocr_shortcut;
        case idx_tool_shortcut_select: return &config.tool_shortcut_select;
        case idx_tool_shortcut_rectangle: return &config.tool_shortcut_rectangle;
        case idx_tool_shortcut_ellipse: return &config.tool_shortcut_ellipse;
        case idx_tool_shortcut_line: return &config.tool_shortcut_line;
        case idx_tool_shortcut_arrow: return &config.tool_shortcut_arrow;
        case idx_tool_shortcut_pen: return &config.tool_shortcut_pen;
        case idx_tool_shortcut_mosaic: return &config.tool_shortcut_mosaic;
        case idx_tool_shortcut_blur: return &config.tool_shortcut_blur;
        case idx_tool_shortcut_highlight: return &config.tool_shortcut_highlight;
        case idx_tool_shortcut_text: return &config.tool_shortcut_text;
        case idx_tool_shortcut_serial: return &config.tool_shortcut_serial;
        case idx_tool_shortcut_eraser: return &config.tool_shortcut_eraser;
        default: return nullptr;
    }
}

constexpr std::array<std::wstring_view, shortcut_count> kShortcutLabels{
    L"截图",
    L"全局 OCR",
    L"选区 OCR",
    L"选择工具",
    L"矩形工具",
    L"椭圆工具",
    L"直线工具",
    L"箭头工具",
    L"画笔工具",
    L"马赛克工具",
    L"模糊工具",
    L"高亮工具",
    L"文本工具",
    L"序号工具",
    L"橡皮擦工具",
};

bool same_hotkey(const Hotkey& left, const Hotkey& right) noexcept {
    constexpr UINT modifier_mask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    return left.virtual_key == right.virtual_key &&
           (left.modifiers & modifier_mask) == (right.modifiers & modifier_mask);
}

std::optional<std::wstring> shortcut_validation_error(const AppConfig& config) {
    std::array<std::optional<Hotkey>, shortcut_count> parsed;
    for (int index = 0; index < shortcut_count; ++index) {
        const std::wstring* value = get_shortcut_ptr(config, index);
        if (!value || value->empty()) {
            continue;
        }
        parsed[static_cast<std::size_t>(index)] = parse_hotkey(*value);
        if (!parsed[static_cast<std::size_t>(index)]) {
            return std::format(L"“{}”的快捷键格式无效。", kShortcutLabels[static_cast<std::size_t>(index)]);
        }
        if (index <= idx_global_ocr_hotkey &&
            (parsed[static_cast<std::size_t>(index)]->modifiers &
             (MOD_CONTROL | MOD_ALT | MOD_WIN)) == 0) {
            return std::format(
                L"“{}”必须包含 Ctrl、Alt 或 Win 修饰键。",
                kShortcutLabels[static_cast<std::size_t>(index)]);
        }
        for (int previous = 0; previous < index; ++previous) {
            const auto& other = parsed[static_cast<std::size_t>(previous)];
            if (other && same_hotkey(*parsed[static_cast<std::size_t>(index)], *other)) {
                return std::format(L"“{}”与“{}”使用了相同的快捷键。",
                                   kShortcutLabels[static_cast<std::size_t>(index)],
                                   kShortcutLabels[static_cast<std::size_t>(previous)]);
            }
        }
    }

    constexpr std::array<std::wstring_view, 4> reserved{
        L"Ctrl+C",
        L"Ctrl+S",
        L"Ctrl+Z",
        L"Ctrl+Y",
    };
    for (int index = idx_capture_ocr_shortcut; index < shortcut_count; ++index) {
        const auto& value = parsed[static_cast<std::size_t>(index)];
        if (!value) {
            continue;
        }
        for (const auto command : reserved) {
            const auto built_in = parse_hotkey(command);
            if (built_in && same_hotkey(*value, *built_in)) {
                return std::format(L"“{}”不能使用截图编辑命令保留的快捷键 {}。",
                                   kShortcutLabels[static_cast<std::size_t>(index)],
                                   command);
            }
        }
    }
    return std::nullopt;
}

constexpr UINT kOcrDownloadStateChanged = WM_APP + 0x120;

struct OcrDownloadSnapshot {
    bool is_downloading{};
    int progress{};
    std::wstring error;
};

class OcrDownloadContext {
public:
    OcrDownloadContext() = default;

    ~OcrDownloadContext() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            subscribers_.clear();
        }
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
    }

    OcrDownloadContext(const OcrDownloadContext&) = delete;
    OcrDownloadContext& operator=(const OcrDownloadContext&) = delete;

    [[nodiscard]] OcrDownloadSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {is_downloading_, progress_, error_};
    }

    [[nodiscard]] UINT_PTR subscribe(HWND window) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++next_subscription_;
        if (next_subscription_ == 0) {
            ++next_subscription_;
        }
        subscribers_.push_back({window, next_subscription_});
        return next_subscription_;
    }

    void unsubscribe(HWND window, UINT_PTR subscription) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::erase_if(
            subscribers_,
            [window, subscription](const Subscription& item) {
                return item.window == window && item.id == subscription;
            });
    }

    void clear_error() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_.empty()) {
            return;
        }
        error_.clear();
        post_update_locked();
    }

    [[nodiscard]] bool start(std::wstring manifest_url) {
        std::jthread completed_worker;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (is_downloading_) {
                return false;
            }
            is_downloading_ = true;
            progress_ = 0;
            error_.clear();
            completed_worker = std::move(worker_);
            post_update_locked();
        }

        if (completed_worker.joinable()) {
            completed_worker.join();
        }

        try {
            std::jthread worker(
                [this, manifest_url = std::move(manifest_url)](
                    std::stop_token stop_token) {
                    std::wstring error;
                    const bool ok = download_ocr_dependencies(
                        manifest_url,
                        [this, stop_token](int progress) {
                            update_progress(progress, stop_token);
                        },
                        &error,
                        stop_token);
                    finish(ok, std::move(error), stop_token);
                });
            std::lock_guard<std::mutex> lock(mutex_);
            worker_ = std::move(worker);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            is_downloading_ = false;
            error_ = L"无法启动 OCR 下载任务。";
            post_update_locked();
            return false;
        }
        return true;
    }

private:
    struct Subscription {
        HWND window{};
        UINT_PTR id{};
    };

    void post_update_locked() const {
        for (const auto& subscriber : subscribers_) {
            PostMessageW(
                subscriber.window,
                kOcrDownloadStateChanged,
                static_cast<WPARAM>(subscriber.id),
                0);
        }
    }

    void update_progress(int progress, std::stop_token stop_token) {
        if (stop_token.stop_requested()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!is_downloading_) {
            return;
        }
        progress_ = std::clamp(progress, 0, 100);
        post_update_locked();
    }

    void finish(bool ok, std::wstring error, std::stop_token stop_token) {
        std::lock_guard<std::mutex> lock(mutex_);
        is_downloading_ = false;
        progress_ = ok ? 100 : progress_;
        error_ = ok ? std::wstring{} : std::move(error);
        if (!stop_token.stop_requested()) {
            post_update_locked();
        }
    }

    mutable std::mutex mutex_;
    bool is_downloading_{};
    int progress_{};
    std::wstring error_;
    std::vector<Subscription> subscribers_;
    UINT_PTR next_subscription_{};
    std::jthread worker_;
};

OcrDownloadContext g_ocr_download;

void refresh_ocr_download_state(
    SettingsState* state,
    bool force_dependency_refresh = false) {
    const bool was_downloading = state->is_downloading;
    OcrDownloadSnapshot snapshot = g_ocr_download.snapshot();
    state->is_downloading = snapshot.is_downloading;
    state->download_progress = snapshot.progress;
    state->download_error = std::move(snapshot.error);
    if (force_dependency_refresh ||
        (was_downloading && !state->is_downloading)) {
        state->ocr_dependency =
            ocr_dependency_status(state->config.ocr_engine);
    }
}

void restore_settings_owner(SettingsState* state) {
    if (state->owner && IsWindow(state->owner)) {
        EnableWindow(state->owner, TRUE);
        SetForegroundWindow(state->owner);
    }
    state->owner = nullptr;
}

void finalize_settings_state(SettingsState* state) {
    if (state->finalized) {
        return;
    }
    state->finalized = true;
    auto completion = std::move(state->completion);
    std::optional<AppConfig> result;
    if (state->accepted) {
        result = std::move(state->config);
    }
    restore_settings_owner(state);
    delete state;
    if (completion) {
        completion(std::move(result));
    }
}

void enter_settings_modal(SettingsState* state) noexcept {
    ++state->modal_depth;
}

bool leave_settings_modal(SettingsState* state) {
    if (state->modal_depth > 0) {
        --state->modal_depth;
    }
    if (state->modal_depth == 0 && state->nc_destroyed) {
        finalize_settings_state(state);
        return false;
    }
    return true;
}

bool accept_settings(SettingsState* state) {
    state->shortcut_error = shortcut_validation_error(state->config);
    if (state->shortcut_error) {
        state->active_tab = 3;
        InvalidateRect(state->window, nullptr, TRUE);
        return true;
    }
    state->accepted = true;
    PostMessageW(state->window, WM_CLOSE, 0, 0);
    return true;
}

bool settings_are_dirty(const SettingsState* state) {
    return config_to_json(state->config) != config_to_json(state->initial_config);
}

struct OcrEngineButton {
    std::wstring_view engine;
    const wchar_t* label;
    int left;
    int right;
};

constexpr std::array<OcrEngineButton, 3> kOcrEngineButtons{{
    {kOcrEngineRapidV5Fast, L"极速", 524, 634},
    {kOcrEngineRapidV5Accurate, L"高精度", 642, 752},
    {kOcrEngineRapidV4Compat, L"兼容", 760, 870},
}};

const OcrEngineButton* hit_test_ocr_engine_button(POINT pt) {
    if (pt.y < 222 || pt.y > 258) {
        return nullptr;
    }
    for (const auto& button : kOcrEngineButtons) {
        if (pt.x >= button.left && pt.x <= button.right) {
            return &button;
        }
    }
    return nullptr;
}

std::vector<std::wstring> split_hidden_tools(std::wstring_view value) {
    std::vector<std::wstring> result;
    std::wstring current;
    for (const wchar_t ch : value) {
        if (ch == L',' || ch == L';' || ch == L'|' || std::iswspace(ch)) {
            if (!current.empty()) {
                result.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        result.push_back(std::move(current));
    }
    return result;
}

bool tool_id_matches(std::wstring_view left, std::wstring_view right) {
    auto to_lower = [](std::wstring_view value) {
        std::wstring result(value);
        std::transform(result.begin(), result.end(), result.begin(), ::towlower);
        return result;
    };
    return to_lower(left) == to_lower(right);
}

void toggle_hidden_tool(std::wstring& hidden_tools, std::wstring_view tool_id) {
    bool is_hidden = annotation_tool_hidden(hidden_tools, tool_id);
    std::vector<std::wstring> tools = split_hidden_tools(hidden_tools);
    if (is_hidden) {
        std::erase_if(tools, [tool_id](const auto& t) { return tool_id_matches(t, tool_id); });
    } else {
        tools.push_back(std::wstring(tool_id));
    }
    std::wstring result;
    for (const auto& t : tools) {
        if (!result.empty()) result += L",";
        result += t;
    }
    hidden_tools = normalize_annotation_hidden_tools(result);
}

std::vector<std::wstring> split_by_comma(std::wstring_view value) {
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
}

std::wstring join_by_comma(const std::vector<std::wstring>& items) {
    std::wstring result;
    for (const auto& t : items) {
        if (!result.empty()) result += L",";
        result += t;
    }
    return result;
}

std::wstring get_tool_display_name(std::wstring_view id) {
    if (id == L"lock") return L"锁定工具";
    if (id == L"select") return L"选择";
    if (id == L"rect") return L"矩形";
    if (id == L"ellipse") return L"椭圆";
    if (id == L"line") return L"直线";
    if (id == L"arrow") return L"箭头";
    if (id == L"pen") return L"画笔";
    if (id == L"mosaic") return L"马赛克";
    if (id == L"blur") return L"模糊";
    if (id == L"highlight") return L"高亮";
    if (id == L"watermark") return L"水印";
    if (id == L"text") return L"文本";
    if (id == L"serial") return L"序号";
    if (id == L"eraser") return L"橡皮擦";
    if (id == L"undo") return L"撤销";
    if (id == L"redo") return L"重做";
    if (id == L"ocr") return L"屏幕识字";
    if (id == L"scroll") return L"长截图";
    if (id == L"pin") return L"贴图";
    if (id == L"copy") return L"复制";
    if (id == L"save") return L"保存";
    if (id == L"close") return L"关闭";
    return std::wstring(id);
}

void discard_resources(SettingsState* state);

D2D1_COLOR_F color_from_system(int index) {
    const COLORREF color = GetSysColor(index);
    return D2D1::ColorF(
        static_cast<float>(GetRValue(color)) / 255.0f,
        static_cast<float>(GetGValue(color)) / 255.0f,
        static_cast<float>(GetBValue(color)) / 255.0f);
}

bool system_high_contrast_enabled() {
    HIGHCONTRASTW high_contrast{sizeof(high_contrast)};
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST, sizeof(high_contrast), &high_contrast, 0) &&
           (high_contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

void refresh_settings_theme(SettingsState* state) {
    const bool light = should_use_light_theme(state->config.theme);
    const bool high_contrast = system_high_contrast_enabled();
    if (state->is_light_theme == light && state->high_contrast == high_contrast &&
        state->render_target) {
        return;
    }
    state->is_light_theme = light;
    state->high_contrast = high_contrast;
    discard_resources(state);
    if (state->window) {
        BOOL use_dark = !light && !high_contrast;
        DwmSetWindowAttribute(state->window, 20, &use_dark, sizeof(use_dark));
        DwmSetWindowAttribute(state->window, 19, &use_dark, sizeof(use_dark));
        InvalidateRect(state->window, nullptr, TRUE);
    }
}

bool ensure_resources(SettingsState* state) {
    if (state->render_target) {
        return true;
    }

    RECT rect{};
    if (!GetClientRect(state->window, &rect) || rect.right <= rect.left ||
        rect.bottom <= rect.top) {
        return false;
    }
    const auto fail = [state] {
        discard_resources(state);
        return false;
    };

    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory),
        nullptr,
        reinterpret_cast<void**>(state->d2d_factory.GetAddressOf()));
    if (FAILED(hr)) {
        return fail();
    }
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(state->dwrite_factory.GetAddressOf()));
    if (FAILED(hr)) {
        return fail();
    }
    hr = state->d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(
            state->window,
            D2D1::SizeU(
                static_cast<UINT32>(rect.right - rect.left),
                static_cast<UINT32>(rect.bottom - rect.top))),
        state->render_target.GetAddressOf());
    if (FAILED(hr)) {
        return fail();
    }
    const float dpi = 96.0f * settings_layout_scale(state->window);
    state->render_target->SetDpi(dpi, dpi);

    struct Palette {
        D2D1_COLOR_F canvas;
        D2D1_COLOR_F rail;
        D2D1_COLOR_F text;
        D2D1_COLOR_F muted;
        D2D1_COLOR_F accent;
        D2D1_COLOR_F accent_hover;
        D2D1_COLOR_F accent_text;
        D2D1_COLOR_F border;
        D2D1_COLOR_F rail_selected;
        D2D1_COLOR_F surface;
        D2D1_COLOR_F switch_off;
        D2D1_COLOR_F separator;
        D2D1_COLOR_F hover;
        D2D1_COLOR_F accent_soft;
        D2D1_COLOR_F danger_soft;
        D2D1_COLOR_F danger;
        D2D1_COLOR_F rail_text;
        D2D1_COLOR_F rail_muted;
        D2D1_COLOR_F cyan;
        D2D1_COLOR_F disabled;
    };

    Palette palette{};
    if (state->high_contrast) {
        palette = {
            color_from_system(COLOR_WINDOW),
            color_from_system(COLOR_WINDOW),
            color_from_system(COLOR_WINDOWTEXT),
            color_from_system(COLOR_GRAYTEXT),
            color_from_system(COLOR_HIGHLIGHT),
            color_from_system(COLOR_HIGHLIGHT),
            color_from_system(COLOR_HIGHLIGHTTEXT),
            color_from_system(COLOR_WINDOWTEXT),
            color_from_system(COLOR_HIGHLIGHT),
            color_from_system(COLOR_WINDOW),
            color_from_system(COLOR_GRAYTEXT),
            color_from_system(COLOR_WINDOWTEXT),
            color_from_system(COLOR_HIGHLIGHT),
            color_from_system(COLOR_HIGHLIGHT),
            color_from_system(COLOR_WINDOW),
            color_from_system(COLOR_HOTLIGHT),
            color_from_system(COLOR_WINDOWTEXT),
            color_from_system(COLOR_GRAYTEXT),
            color_from_system(COLOR_HIGHLIGHT),
            color_from_system(COLOR_BTNFACE),
        };
    } else if (state->is_light_theme) {
        palette = {
            D2D1::ColorF(0xF6F8FC),
            D2D1::ColorF(0x091426),
            D2D1::ColorF(0x152033),
            D2D1::ColorF(0x63708A),
            D2D1::ColorF(0x2764E7),
            D2D1::ColorF(0x1E55C8),
            D2D1::ColorF(0xFFFFFF),
            D2D1::ColorF(0xDCE3EE),
            D2D1::ColorF(0x112441),
            D2D1::ColorF(0xFFFFFF),
            D2D1::ColorF(0x97A5BC),
            D2D1::ColorF(0xE7ECF3),
            D2D1::ColorF(0xEFF4FD),
            D2D1::ColorF(0xE8F0FF),
            D2D1::ColorF(0xFCE8EA),
            D2D1::ColorF(0xC8374D),
            D2D1::ColorF(0xF5F8FF),
            D2D1::ColorF(0x8D9CB5),
            D2D1::ColorF(0x1AAFB5),
            D2D1::ColorF(0xEDF1F6),
        };
    } else {
        palette = {
            D2D1::ColorF(0x0F141D),
            D2D1::ColorF(0x070B12),
            D2D1::ColorF(0xF3F6FB),
            D2D1::ColorF(0x97A3B6),
            D2D1::ColorF(0x5B8EFF),
            D2D1::ColorF(0x75A1FF),
            D2D1::ColorF(0x07101E),
            D2D1::ColorF(0x293241),
            D2D1::ColorF(0x101E36),
            D2D1::ColorF(0x171E29),
            D2D1::ColorF(0x536074),
            D2D1::ColorF(0x27303E),
            D2D1::ColorF(0x1A2433),
            D2D1::ColorF(0x1B2D50),
            D2D1::ColorF(0x3B1E25),
            D2D1::ColorF(0xFF7185),
            D2D1::ColorF(0xF4F7FC),
            D2D1::ColorF(0x8A96A9),
            D2D1::ColorF(0x42D5DD),
            D2D1::ColorF(0x202735),
        };
    }

    auto create_brush = [&](D2D1_COLOR_F color,
                            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& brush) {
        return state->render_target->CreateSolidColorBrush(
            color, brush.ReleaseAndGetAddressOf());
    };
    hr = create_brush(palette.canvas, state->bg_brush);
    hr = SUCCEEDED(hr) ? create_brush(palette.rail, state->sidebar_bg_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.text, state->text_white_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.muted, state->text_grey_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.accent, state->blue_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.accent_hover, state->hover_blue_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.accent_text, state->accent_text_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.border, state->border_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.rail_selected, state->active_tab_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.surface, state->control_bg_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.switch_off, state->switch_track_off_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.surface, state->card_bg_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.border, state->card_border_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.separator, state->separator_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.hover, state->hover_bg_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.border, state->cancel_btn_border_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.accent, state->accent_indicator_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.border, state->keycap_shadow_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.accent_soft, state->switch_glow_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.danger_soft, state->close_btn_hover_bg_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.danger, state->red_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.rail_text, state->rail_text_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.rail_muted, state->rail_muted_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.accent_soft, state->accent_soft_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.cyan, state->cyan_brush) : hr;
    hr = SUCCEEDED(hr) ? create_brush(palette.disabled, state->disabled_bg_brush) : hr;
    if (FAILED(hr)) {
        return fail();
    }

    constexpr wchar_t ui_font[] = L"Segoe UI";
    auto create_format = [&](const wchar_t* family,
                             DWRITE_FONT_WEIGHT weight,
                             float size,
                             Microsoft::WRL::ComPtr<IDWriteTextFormat>& format) {
        HRESULT format_hr = state->dwrite_factory->CreateTextFormat(
            family,
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size,
            L"zh-CN",
            format.ReleaseAndGetAddressOf());
        if (SUCCEEDED(format_hr)) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        return format_hr;
    };
    hr = create_format(ui_font, DWRITE_FONT_WEIGHT_SEMI_BOLD, 22.0f, state->title_format);
    hr = SUCCEEDED(hr)
             ? create_format(ui_font, DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.0f, state->section_format)
             : hr;
    hr = SUCCEEDED(hr)
             ? create_format(ui_font, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, state->text_format)
             : hr;
    hr = SUCCEEDED(hr)
             ? create_format(ui_font, DWRITE_FONT_WEIGHT_NORMAL, 12.0f, state->small_format)
             : hr;
    hr = SUCCEEDED(hr)
             ? create_format(ui_font, DWRITE_FONT_WEIGHT_NORMAL, 11.0f, state->utility_format)
             : hr;
    hr = SUCCEEDED(hr)
             ? create_format(ui_font, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, state->rail_format)
             : hr;
    hr = SUCCEEDED(hr)
             ? create_format(L"Consolas", DWRITE_FONT_WEIGHT_MEDIUM, 12.0f, state->hotkey_format)
             : hr;
    hr = SUCCEEDED(hr)
             ? create_format(ui_font, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, state->btn_text_format)
             : hr;
    if (FAILED(hr)) {
        return fail();
    }
    state->hotkey_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    state->btn_text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    return true;
}

void discard_resources(SettingsState* state) {
    state->render_target.Reset();
    state->d2d_factory.Reset();
    state->dwrite_factory.Reset();
    state->bg_brush.Reset();
    state->sidebar_bg_brush.Reset();
    state->text_white_brush.Reset();
    state->text_grey_brush.Reset();
    state->blue_brush.Reset();
    state->hover_blue_brush.Reset();
    state->accent_text_brush.Reset();
    state->border_brush.Reset();
    state->active_tab_brush.Reset();
    state->control_bg_brush.Reset();
    state->switch_track_off_brush.Reset();
    state->card_bg_brush.Reset();
    state->card_border_brush.Reset();
    state->separator_brush.Reset();
    state->hover_bg_brush.Reset();
    state->cancel_btn_border_brush.Reset();
    state->accent_indicator_brush.Reset();
    state->keycap_shadow_brush.Reset();
    state->switch_glow_brush.Reset();
    state->close_btn_hover_bg_brush.Reset();
    state->red_brush.Reset();
    state->rail_text_brush.Reset();
    state->rail_muted_brush.Reset();
    state->accent_soft_brush.Reset();
    state->cyan_brush.Reset();
    state->disabled_bg_brush.Reset();
    state->title_format.Reset();
    state->section_format.Reset();
    state->text_format.Reset();
    state->small_format.Reset();
    state->utility_format.Reset();
    state->rail_format.Reset();
    state->hotkey_format.Reset();
    state->btn_text_format.Reset();
}

enum ButtonStyle {
    btn_primary,
    btn_secondary,
};

bool point_in_rect(POINT point, float left, float top, float right, float bottom) {
    return point.x >= left && point.x <= right && point.y >= top && point.y <= bottom;
}

void draw_switch(
    SettingsState* state,
    int x,
    int y,
    bool is_on,
    bool enabled = true,
    bool hovered = false) {
    const D2D1_RECT_F rect = D2D1::RectF(
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(x + 42),
        static_cast<float>(y + 24));
    const auto rounded = D2D1::RoundedRect(rect, 12.0f, 12.0f);
    ID2D1SolidColorBrush* track = !enabled
                                     ? state->disabled_bg_brush.Get()
                                     : (is_on ? state->blue_brush.Get()
                                              : state->switch_track_off_brush.Get());
    state->render_target->FillRoundedRectangle(rounded, track);
    if (hovered && enabled) {
        state->render_target->DrawRoundedRectangle(
            rounded, state->hover_blue_brush.Get(), 1.0f);
    }
    const float thumb_x = is_on ? x + 30.0f : x + 12.0f;
    ID2D1SolidColorBrush* thumb =
        enabled && is_on ? state->accent_text_brush.Get()
                         : state->rail_text_brush.Get();
    state->render_target->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(thumb_x, y + 12.0f), 9.0f, 9.0f),
        thumb);
}

void draw_button(
    SettingsState* state,
    int x1,
    int y1,
    int x2,
    int y2,
    const wchar_t* label,
    bool is_hovered,
    ButtonStyle style = btn_primary,
    bool enabled = true) {
    const D2D1_RECT_F rect = D2D1::RectF(
        static_cast<float>(x1),
        static_cast<float>(y1),
        static_cast<float>(x2),
        static_cast<float>(y2));
    const auto rounded = D2D1::RoundedRect(rect, 8.0f, 8.0f);
    if (!enabled) {
        state->render_target->FillRoundedRectangle(rounded, state->disabled_bg_brush.Get());
        state->render_target->DrawRoundedRectangle(
            rounded, state->card_border_brush.Get(), 1.0f);
        state->render_target->DrawTextW(
            label,
            static_cast<UINT32>(wcslen(label)),
            state->btn_text_format.Get(),
            rect,
            state->text_grey_brush.Get());
        return;
    }
    if (style == btn_primary) {
        state->render_target->FillRoundedRectangle(
            rounded, is_hovered ? state->hover_blue_brush.Get() : state->blue_brush.Get());
        state->render_target->DrawTextW(
            label,
            static_cast<UINT32>(wcslen(label)),
            state->btn_text_format.Get(),
            rect,
            state->accent_text_brush.Get());
    } else {
        state->render_target->FillRoundedRectangle(
            rounded, is_hovered ? state->hover_bg_brush.Get() : state->control_bg_brush.Get());
        state->render_target->DrawRoundedRectangle(
            rounded,
            is_hovered ? state->hover_blue_brush.Get()
                       : state->cancel_btn_border_brush.Get(),
            1.0f);
        state->render_target->DrawTextW(
            label,
            static_cast<UINT32>(wcslen(label)),
            state->btn_text_format.Get(),
            rect,
            state->text_white_brush.Get());
    }
}

void draw_choice_button(
    SettingsState* state,
    int x1,
    int y1,
    int x2,
    int y2,
    const wchar_t* label,
    bool is_selected,
    bool is_hovered) {
    const D2D1_RECT_F rect = D2D1::RectF(
        static_cast<float>(x1),
        static_cast<float>(y1),
        static_cast<float>(x2),
        static_cast<float>(y2));
    const auto rounded = D2D1::RoundedRect(rect, 7.0f, 7.0f);
    state->render_target->FillRoundedRectangle(
        rounded,
        is_selected ? state->accent_soft_brush.Get()
                    : (is_hovered ? state->hover_bg_brush.Get()
                                  : state->control_bg_brush.Get()));
    state->render_target->DrawRoundedRectangle(
        rounded,
        is_selected ? state->hover_blue_brush.Get()
                    : state->card_border_brush.Get(),
        1.0f);
    state->render_target->DrawTextW(
        label,
        static_cast<UINT32>(wcslen(label)),
        state->btn_text_format.Get(),
        rect,
        is_selected ? state->hover_blue_brush.Get()
                    : state->text_grey_brush.Get());
}

void draw_hotkey_box(
    SettingsState* state,
    int x1,
    int y1,
    int x2,
    int y2,
    const wchar_t* hotkey,
    bool is_capturing,
    bool is_hovered) {
    const D2D1_RECT_F rect = D2D1::RectF(
        static_cast<float>(x1),
        static_cast<float>(y1),
        static_cast<float>(x2),
        static_cast<float>(y2));
    const auto rounded = D2D1::RoundedRect(rect, 7.0f, 7.0f);
    state->render_target->FillRoundedRectangle(
        rounded, is_capturing ? state->accent_soft_brush.Get()
                              : state->control_bg_brush.Get());
    state->render_target->DrawRoundedRectangle(
        rounded,
        is_capturing ? state->hover_blue_brush.Get()
                     : (is_hovered ? state->hover_blue_brush.Get()
                                   : state->card_border_brush.Get()),
        is_capturing ? 1.5f : 1.0f);
    const wchar_t* text = is_capturing ? L"请按组合键 · Esc 取消"
                                       : (hotkey && *hotkey ? hotkey : L"未设置");
    state->render_target->DrawTextW(
        text,
        static_cast<UINT32>(wcslen(text)),
        state->hotkey_format.Get(),
        rect,
        is_capturing ? state->hover_blue_brush.Get()
                     : state->text_white_brush.Get());
}

void draw_nav_icon(
    SettingsState* state,
    int category,
    float x,
    float y,
    ID2D1SolidColorBrush* brush) {
    auto* target = state->render_target.Get();
    const float stroke = 1.5f;
    if (category == 0) {
        target->DrawLine(D2D1::Point2F(x, y + 6), D2D1::Point2F(x, y), brush, stroke);
        target->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x + 6, y), brush, stroke);
        target->DrawLine(D2D1::Point2F(x + 12, y), D2D1::Point2F(x + 18, y), brush, stroke);
        target->DrawLine(D2D1::Point2F(x + 18, y), D2D1::Point2F(x + 18, y + 6), brush, stroke);
        target->DrawLine(D2D1::Point2F(x, y + 12), D2D1::Point2F(x, y + 18), brush, stroke);
        target->DrawLine(D2D1::Point2F(x, y + 18), D2D1::Point2F(x + 6, y + 18), brush, stroke);
        target->DrawLine(D2D1::Point2F(x + 12, y + 18), D2D1::Point2F(x + 18, y + 18), brush, stroke);
        target->DrawLine(D2D1::Point2F(x + 18, y + 12), D2D1::Point2F(x + 18, y + 18), brush, stroke);
    } else if (category == 1) {
        target->DrawLine(D2D1::Point2F(x + 2, y + 2), D2D1::Point2F(x + 16, y + 2), brush, stroke);
        target->DrawLine(D2D1::Point2F(x + 9, y + 2), D2D1::Point2F(x + 9, y + 16), brush, stroke);
        target->DrawLine(D2D1::Point2F(x + 5, y + 16), D2D1::Point2F(x + 13, y + 16), brush, stroke);
    } else if (category == 2) {
        for (int row = 0; row < 3; ++row) {
            const float yy = y + 3.0f + row * 6.0f;
            target->DrawLine(D2D1::Point2F(x + 1, yy), D2D1::Point2F(x + 17, yy), brush, stroke);
            const float knob = row == 0 ? x + 6.0f : (row == 1 ? x + 13.0f : x + 9.0f);
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob, yy), 2.0f, 2.0f), brush);
        }
    } else if (category == 3) {
        const auto rect = D2D1::RoundedRect(D2D1::RectF(x, y + 2, x + 18, y + 16), 3.0f, 3.0f);
        target->DrawRoundedRectangle(rect, brush, stroke);
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 3; ++column) {
                target->FillRectangle(
                    D2D1::RectF(
                        x + 3 + column * 5.0f,
                        y + 5 + row * 5.0f,
                        x + 5 + column * 5.0f,
                        y + 7 + row * 5.0f),
                    brush);
            }
        }
    } else {
        target->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(x + 9, y + 9), 4.0f, 4.0f), brush, stroke);
        for (int index = 0; index < 8; ++index) {
            const float angle = static_cast<float>(index) * 3.14159265f / 4.0f;
            target->DrawLine(
                D2D1::Point2F(x + 9 + std::cos(angle) * 7.0f, y + 9 + std::sin(angle) * 7.0f),
                D2D1::Point2F(x + 9 + std::cos(angle) * 9.0f, y + 9 + std::sin(angle) * 9.0f),
                brush,
                stroke);
        }
    }
}

void draw_page_header(
    SettingsState* state,
    std::wstring_view title,
    std::wstring_view helper) {
    state->render_target->DrawTextW(
        title.data(),
        static_cast<UINT32>(title.size()),
        state->title_format.Get(),
        D2D1::RectF(256.0f, 66.0f, 888.0f, 100.0f),
        state->text_white_brush.Get());
    state->render_target->DrawTextW(
        helper.data(),
        static_cast<UINT32>(helper.size()),
        state->small_format.Get(),
        D2D1::RectF(256.0f, 98.0f, 888.0f, 122.0f),
        state->text_grey_brush.Get());
}

void draw_section_title(SettingsState* state, std::wstring_view title, float top) {
    state->render_target->DrawTextW(
        title.data(),
        static_cast<UINT32>(title.size()),
        state->section_format.Get(),
        D2D1::RectF(256.0f, top, 888.0f, top + 26.0f),
        state->text_white_brush.Get());
}

void draw_row_group(SettingsState* state, float top, float bottom) {
    const auto group = D2D1::RoundedRect(
        D2D1::RectF(256.0f, top, 888.0f, bottom), 10.0f, 10.0f);
    state->render_target->FillRoundedRectangle(group, state->card_bg_brush.Get());
    state->render_target->DrawRoundedRectangle(group, state->card_border_brush.Get(), 1.0f);
}

void draw_setting_row(
    SettingsState* state,
    float top,
    float bottom,
    std::wstring_view label,
    std::wstring_view helper,
    bool hovered,
    bool enabled = true,
    bool draw_separator = true) {
    if (hovered && enabled) {
        state->render_target->FillRectangle(
            D2D1::RectF(257.0f, top + 1.0f, 887.0f, bottom - 1.0f),
            state->hover_bg_brush.Get());
    }
    ID2D1SolidColorBrush* label_brush =
        enabled ? state->text_white_brush.Get() : state->text_grey_brush.Get();
    state->render_target->DrawTextW(
        label.data(),
        static_cast<UINT32>(label.size()),
        state->text_format.Get(),
        D2D1::RectF(276.0f, top + 6.0f, 610.0f, top + 30.0f),
        label_brush);
    if (!helper.empty()) {
        state->render_target->DrawTextW(
            helper.data(),
            static_cast<UINT32>(helper.size()),
            state->small_format.Get(),
            D2D1::RectF(276.0f, top + 28.0f, 650.0f, bottom - 4.0f),
            state->text_grey_brush.Get());
    }
    if (draw_separator) {
        state->render_target->DrawLine(
            D2D1::Point2F(276.0f, bottom),
            D2D1::Point2F(868.0f, bottom),
            state->separator_brush.Get(),
            1.0f);
    }
}

std::vector<SettingsFocusTarget> settings_focus_targets(const SettingsState* state) {
    std::vector<SettingsFocusTarget> targets;
    targets.reserve(40);
    for (int index = 0; index < kCategoryCount; ++index) {
        targets.push_back({SettingsFocusKind::category, index});
    }
    if (state->active_tab == 0) {
        targets.push_back({SettingsFocusKind::capture_annotation});
        if (state->config.annotation_enabled) {
            targets.push_back({SettingsFocusKind::capture_locked_tool});
        }
        targets.push_back({SettingsFocusKind::capture_output, 0});
        targets.push_back({SettingsFocusKind::capture_output, 1});
        targets.push_back({SettingsFocusKind::capture_notifications});
        if (state->config.annotation_next_serial != 1) {
            targets.push_back({SettingsFocusKind::reset_serial});
        }
    } else if (state->active_tab == 1) {
        targets.push_back({SettingsFocusKind::ocr_enabled});
        if (state->config.ocr_enabled && !state->is_downloading) {
            for (int index = 0; index < static_cast<int>(kOcrEngineButtons.size()); ++index) {
                targets.push_back({SettingsFocusKind::ocr_engine, index});
            }
            if (state->ocr_dependency.can_download) {
                targets.push_back({SettingsFocusKind::download_ocr});
            }
        }
        targets.push_back({SettingsFocusKind::font_family});
        targets.push_back({SettingsFocusKind::text_bold});
        targets.push_back({SettingsFocusKind::text_italic});
    } else if (state->active_tab == 2) {
        const auto tools = split_by_comma(state->config.toolbar_order);
        for (int index = 0; index < static_cast<int>(tools.size()); ++index) {
            targets.push_back({SettingsFocusKind::toolbar_item, index});
        }
        if (state->selected_tool_idx >= 0 &&
            state->selected_tool_idx < static_cast<int>(tools.size())) {
            targets.push_back({SettingsFocusKind::toolbar_visibility});
            if (state->selected_tool_idx > 0) {
                targets.push_back({SettingsFocusKind::toolbar_move_up});
            }
            if (state->selected_tool_idx + 1 < static_cast<int>(tools.size())) {
                targets.push_back({SettingsFocusKind::toolbar_move_down});
            }
        }
    } else if (state->active_tab == 3) {
        if (state->config.ocr_enabled) {
            targets.push_back({SettingsFocusKind::global_ocr_enabled});
        }
        for (int index = 0; index < shortcut_count; ++index) {
            targets.push_back({SettingsFocusKind::shortcut, index});
        }
    } else {
        targets.push_back({SettingsFocusKind::app_shell});
        if (state->config.shell_enabled) {
            targets.push_back({SettingsFocusKind::app_startup});
        }
        for (int index = 0; index < 3; ++index) {
            targets.push_back({SettingsFocusKind::theme, index});
        }
        for (int index = 0; index < 3; ++index) {
            targets.push_back({SettingsFocusKind::app_icon, index});
        }
    }
    if (settings_are_dirty(state) && !state->shortcut_error) {
        targets.push_back({SettingsFocusKind::save});
    }
    targets.push_back({SettingsFocusKind::cancel});
    targets.push_back({SettingsFocusKind::close});
    return targets;
}

bool settings_focus_target_available(
    const SettingsState* state,
    const SettingsFocusTarget& target) {
    const auto targets = settings_focus_targets(state);
    return std::ranges::find(targets, target) != targets.end();
}

std::optional<D2D1_ROUNDED_RECT> settings_focus_bounds(
    const SettingsState* state,
    const SettingsFocusTarget& target) {
    D2D1_RECT_F rect{};
    float radius = 8.0f;
    switch (target.kind) {
        case SettingsFocusKind::category: {
            const float top = static_cast<float>(kCategoryTop + target.index * kCategoryPitch);
            rect = D2D1::RectF(12.0f, top, 212.0f, top + 40.0f);
            break;
        }
        case SettingsFocusKind::capture_annotation:
            rect = D2D1::RectF(258.0f, 226.0f, 886.0f, 282.0f);
            break;
        case SettingsFocusKind::capture_locked_tool:
            rect = D2D1::RectF(258.0f, 282.0f, 886.0f, 338.0f);
            break;
        case SettingsFocusKind::capture_output:
            rect = target.index == 0 ? D2D1::RectF(594.0f, 429.0f, 724.0f, 465.0f)
                                     : D2D1::RectF(732.0f, 429.0f, 870.0f, 465.0f);
            break;
        case SettingsFocusKind::capture_notifications:
            rect = D2D1::RectF(258.0f, 478.0f, 886.0f, 534.0f);
            break;
        case SettingsFocusKind::reset_serial:
            rect = D2D1::RectF(766.0f, 544.0f, 870.0f, 580.0f);
            break;
        case SettingsFocusKind::ocr_enabled:
            rect = D2D1::RectF(258.0f, 150.0f, 886.0f, 206.0f);
            break;
        case SettingsFocusKind::ocr_engine:
            if (target.index < 0 ||
                target.index >= static_cast<int>(kOcrEngineButtons.size())) {
                return std::nullopt;
            }
            rect = D2D1::RectF(
                static_cast<float>(kOcrEngineButtons[static_cast<std::size_t>(target.index)].left),
                222.0f,
                static_cast<float>(kOcrEngineButtons[static_cast<std::size_t>(target.index)].right),
                258.0f);
            break;
        case SettingsFocusKind::download_ocr:
            rect = D2D1::RectF(746.0f, 294.0f, 870.0f, 330.0f);
            break;
        case SettingsFocusKind::font_family:
            rect = D2D1::RectF(630.0f, 428.0f, 870.0f, 464.0f);
            break;
        case SettingsFocusKind::text_bold:
            rect = D2D1::RectF(736.0f, 484.0f, 796.0f, 520.0f);
            break;
        case SettingsFocusKind::text_italic:
            rect = D2D1::RectF(808.0f, 484.0f, 870.0f, 520.0f);
            break;
        case SettingsFocusKind::toolbar_item: {
            const int visible_index = target.index - state->toolbar_scroll_offset;
            if (visible_index < 0 || visible_index >= kToolbarVisibleRows) {
                return std::nullopt;
            }
            const float top = 160.0f + visible_index * kToolbarRowHeight;
            rect = D2D1::RectF(258.0f, top, 646.0f, top + 38.0f);
            radius = 6.0f;
            break;
        }
        case SettingsFocusKind::toolbar_visibility:
            rect = D2D1::RectF(674.0f, 190.0f, 884.0f, 246.0f);
            break;
        case SettingsFocusKind::toolbar_move_up:
            rect = D2D1::RectF(674.0f, 274.0f, 776.0f, 312.0f);
            break;
        case SettingsFocusKind::toolbar_move_down:
            rect = D2D1::RectF(784.0f, 274.0f, 886.0f, 312.0f);
            break;
        case SettingsFocusKind::global_ocr_enabled:
            rect = D2D1::RectF(258.0f, 192.0f, 886.0f, 244.0f);
            break;
        case SettingsFocusKind::shortcut: {
            if (target.index < 0 || target.index >= shortcut_count) {
                return std::nullopt;
            }
            if (target.index < 3) {
                const float top = 244.0f + target.index * 52.0f;
                rect = D2D1::RectF(684.0f, top + 8.0f, 870.0f, top + 44.0f);
            } else {
                const int tool_index = target.index - 3;
                const int column = tool_index / 6;
                const int row = tool_index % 6;
                const float left = column == 0 ? 420.0f : 736.0f;
                const float top = 436.0f + row * 36.0f;
                rect = D2D1::RectF(left, top + 3.0f, left + 134.0f, top + 33.0f);
            }
            break;
        }
        case SettingsFocusKind::app_shell:
            rect = D2D1::RectF(258.0f, 152.0f, 886.0f, 208.0f);
            break;
        case SettingsFocusKind::app_startup:
            rect = D2D1::RectF(258.0f, 208.0f, 886.0f, 264.0f);
            break;
        case SettingsFocusKind::theme: {
            constexpr std::array<std::pair<int, int>, 3> bounds{{
                {486, 608},
                {616, 738},
                {746, 870},
            }};
            if (target.index < 0 || target.index >= static_cast<int>(bounds.size())) {
                return std::nullopt;
            }
            const auto [left, right] = bounds[static_cast<std::size_t>(target.index)];
            rect = D2D1::RectF(
                static_cast<float>(left), 344.0f, static_cast<float>(right), 382.0f);
            break;
        }
        case SettingsFocusKind::app_icon: {
            constexpr std::array<std::pair<int, int>, 3> bounds{{
                {276, 468},
                {476, 668},
                {676, 868},
            }};
            if (target.index < 0 || target.index >= static_cast<int>(bounds.size())) {
                return std::nullopt;
            }
            const auto [left, right] = bounds[static_cast<std::size_t>(target.index)];
            rect = D2D1::RectF(
                static_cast<float>(left), 470.0f, static_cast<float>(right), 552.0f);
            break;
        }
        case SettingsFocusKind::save:
            rect = D2D1::RectF(776.0f, 670.0f, 888.0f, 710.0f);
            break;
        case SettingsFocusKind::cancel:
            rect = D2D1::RectF(672.0f, 670.0f, 764.0f, 710.0f);
            break;
        case SettingsFocusKind::close:
            rect = D2D1::RectF(872.0f, 2.0f, 918.0f, 46.0f);
            radius = 2.0f;
            break;
    }
    return D2D1::RoundedRect(rect, radius, radius);
}

void draw_settings_keyboard_focus(SettingsState* state) {
    if (!state->keyboard_focus_visible || !state->keyboard_focus ||
        !settings_focus_target_available(state, *state->keyboard_focus)) {
        return;
    }
    auto bounds = settings_focus_bounds(state, *state->keyboard_focus);
    if (!bounds) {
        return;
    }
    bounds->rect.left = std::max(2.0f, bounds->rect.left - 2.0f);
    bounds->rect.top = std::max(2.0f, bounds->rect.top - 2.0f);
    bounds->rect.right = std::min(918.0f, bounds->rect.right + 2.0f);
    bounds->rect.bottom = std::min(718.0f, bounds->rect.bottom + 2.0f);
    bounds->radiusX += 2.0f;
    bounds->radiusY += 2.0f;
    state->render_target->DrawRoundedRectangle(
        *bounds, state->accent_indicator_brush.Get(), 2.0f);
}

void move_settings_keyboard_focus(SettingsState* state, bool backwards) {
    const auto targets = settings_focus_targets(state);
    if (targets.empty()) {
        state->keyboard_focus.reset();
        state->keyboard_focus_visible = false;
        return;
    }
    auto current = targets.end();
    if (state->keyboard_focus) {
        current = std::ranges::find(targets, *state->keyboard_focus);
    }
    if (current == targets.end()) {
        state->keyboard_focus = backwards ? targets.back() : targets.front();
    } else if (backwards) {
        state->keyboard_focus =
            current == targets.begin() ? targets.back() : *std::prev(current);
    } else {
        state->keyboard_focus =
            std::next(current) == targets.end() ? targets.front() : *std::next(current);
    }
    if (state->keyboard_focus &&
        state->keyboard_focus->kind == SettingsFocusKind::toolbar_item) {
        const int index = state->keyboard_focus->index;
        if (index < state->toolbar_scroll_offset) {
            state->toolbar_scroll_offset = index;
        } else if (index >= state->toolbar_scroll_offset + kToolbarVisibleRows) {
            state->toolbar_scroll_offset = index - kToolbarVisibleRows + 1;
        }
    }
    state->keyboard_focus_visible = true;
    state->capturing_idx_ = -1;
    InvalidateRect(state->window, nullptr, FALSE);
}

bool show_font_family_menu(SettingsState* state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return true;
    }
    constexpr std::array<const wchar_t*, 7> fonts{
        L"Microsoft YaHei",
        L"Consolas",
        L"SimSun",
        L"SimHei",
        L"KaiTi",
        L"Arial",
        L"Segoe UI",
    };
    for (int index = 0; index < static_cast<int>(fonts.size()); ++index) {
        UINT flags = MF_STRING;
        if (state->config.text_font_family == fonts[static_cast<std::size_t>(index)]) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(menu, flags, 1000 + index, fonts[static_cast<std::size_t>(index)]);
    }

    POINT screen_point{
        settings_scale_dip(state->window, 630),
        settings_scale_dip(state->window, 462),
    };
    ClientToScreen(state->window, &screen_point);
    enter_settings_modal(state);
    const int selection = TrackPopupMenu(
        menu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        screen_point.x,
        screen_point.y,
        0,
        state->window,
        nullptr);
    DestroyMenu(menu);
    if (!leave_settings_modal(state)) {
        return false;
    }
    if (selection >= 1000 &&
        selection < 1000 + static_cast<int>(fonts.size())) {
        state->config.text_font_family =
            fonts[static_cast<std::size_t>(selection - 1000)];
        InvalidateRect(state->window, nullptr, TRUE);
    }
    return true;
}

bool activate_settings_focus(
    SettingsState* state,
    const SettingsFocusTarget& target) {
    switch (target.kind) {
        case SettingsFocusKind::category:
            if (target.index >= 0 && target.index < kCategoryCount) {
                state->active_tab = target.index;
                state->capturing_idx_ = -1;
            }
            break;
        case SettingsFocusKind::capture_annotation:
            state->config.annotation_enabled = !state->config.annotation_enabled;
            break;
        case SettingsFocusKind::capture_locked_tool:
            if (state->config.annotation_enabled) {
                state->config.annotation_locked_tool =
                    !state->config.annotation_locked_tool;
            }
            break;
        case SettingsFocusKind::capture_output:
            state->config.default_output = target.index == 1 ? L"file" : L"clipboard";
            break;
        case SettingsFocusKind::capture_notifications:
            state->config.notifications_enabled =
                !state->config.notifications_enabled;
            break;
        case SettingsFocusKind::reset_serial:
            state->config.annotation_next_serial = 1;
            break;
        case SettingsFocusKind::ocr_enabled:
            state->config.ocr_enabled = !state->config.ocr_enabled;
            if (!state->config.ocr_enabled) {
                state->config.global_ocr_enabled = false;
            }
            refresh_ocr_download_state(state, true);
            break;
        case SettingsFocusKind::font_family:
            return show_font_family_menu(state);
        case SettingsFocusKind::text_bold:
            state->config.text_font_bold = !state->config.text_font_bold;
            break;
        case SettingsFocusKind::text_italic:
            state->config.text_font_italic = !state->config.text_font_italic;
            break;
        case SettingsFocusKind::ocr_engine:
            if (!state->is_downloading && target.index >= 0 &&
                target.index < static_cast<int>(kOcrEngineButtons.size())) {
                state->config.ocr_engine = std::wstring(
                    kOcrEngineButtons[static_cast<std::size_t>(target.index)].engine);
                g_ocr_download.clear_error();
                refresh_ocr_download_state(state, true);
            }
            break;
        case SettingsFocusKind::download_ocr: {
            const OcrDependencyStatus status =
                ocr_dependency_status(state->config.ocr_engine);
            state->ocr_dependency = status;
            if (status.can_download && !state->is_downloading) {
                const bool started =
                    g_ocr_download.start(state->config.ocr_download_url);
                refresh_ocr_download_state(state);
                if (!started && state->download_error.empty()) {
                    state->download_error = L"OCR 下载任务已在运行。";
                }
            }
            break;
        }
        case SettingsFocusKind::toolbar_item: {
            const auto tools = split_by_comma(state->config.toolbar_order);
            if (target.index >= 0 && target.index < static_cast<int>(tools.size())) {
                state->selected_tool_idx = target.index;
            }
            break;
        }
        case SettingsFocusKind::toolbar_visibility: {
            const auto tools = split_by_comma(state->config.toolbar_order);
            if (state->selected_tool_idx >= 0 &&
                state->selected_tool_idx < static_cast<int>(tools.size())) {
                toggle_hidden_tool(
                    state->config.annotation_hidden_tools,
                    tools[static_cast<std::size_t>(state->selected_tool_idx)]);
            }
            break;
        }
        case SettingsFocusKind::toolbar_move_up:
        case SettingsFocusKind::toolbar_move_down: {
            auto tools = split_by_comma(state->config.toolbar_order);
            const int selected = state->selected_tool_idx;
            const int delta =
                target.kind == SettingsFocusKind::toolbar_move_up ? -1 : 1;
            const int destination = selected + delta;
            if (selected >= 0 && destination >= 0 &&
                selected < static_cast<int>(tools.size()) &&
                destination < static_cast<int>(tools.size())) {
                std::swap(
                    tools[static_cast<std::size_t>(selected)],
                    tools[static_cast<std::size_t>(destination)]);
                state->config.toolbar_order = join_by_comma(tools);
                state->selected_tool_idx = destination;
                if (destination < state->toolbar_scroll_offset) {
                    state->toolbar_scroll_offset = destination;
                } else if (
                    destination >= state->toolbar_scroll_offset + kToolbarVisibleRows) {
                    state->toolbar_scroll_offset =
                        destination - kToolbarVisibleRows + 1;
                }
                const bool reached_boundary =
                    (target.kind == SettingsFocusKind::toolbar_move_up &&
                     destination == 0) ||
                    (target.kind == SettingsFocusKind::toolbar_move_down &&
                     destination + 1 == static_cast<int>(tools.size()));
                if (reached_boundary) {
                    state->keyboard_focus =
                        SettingsFocusTarget{SettingsFocusKind::toolbar_item, destination};
                }
            }
            break;
        }
        case SettingsFocusKind::global_ocr_enabled:
            if (state->config.ocr_enabled) {
                state->config.global_ocr_enabled =
                    !state->config.global_ocr_enabled;
            }
            break;
        case SettingsFocusKind::shortcut:
            if (target.index >= 0 && target.index < shortcut_count) {
                state->capturing_idx_ = target.index;
            }
            break;
        case SettingsFocusKind::app_shell:
            state->config.shell_enabled = !state->config.shell_enabled;
            break;
        case SettingsFocusKind::app_startup:
            if (state->config.shell_enabled) {
                state->config.start_at_login = !state->config.start_at_login;
            }
            break;
        case SettingsFocusKind::theme: {
            constexpr std::array<std::wstring_view, 3> themes{
                L"system",
                L"light",
                L"dark",
            };
            if (target.index >= 0 && target.index < static_cast<int>(themes.size())) {
                state->config.theme =
                    std::wstring(themes[static_cast<std::size_t>(target.index)]);
                refresh_settings_theme(state);
            }
            break;
        }
        case SettingsFocusKind::app_icon: {
            constexpr std::array<std::wstring_view, 3> icons{
                kAppIconFocusFrame,
                kAppIconFlowLens,
                kAppIconPixelConsole,
            };
            if (target.index >= 0 && target.index < static_cast<int>(icons.size())) {
                state->config.app_icon =
                    std::wstring(icons[static_cast<std::size_t>(target.index)]);
            }
            break;
        }
        case SettingsFocusKind::save:
            if (settings_are_dirty(state) && !state->shortcut_error) {
                return accept_settings(state);
            }
            break;
        case SettingsFocusKind::cancel:
        case SettingsFocusKind::close:
            PostMessageW(state->window, WM_CLOSE, 0, 0);
            return true;
    }
    state->shortcut_error = shortcut_validation_error(state->config);
    InvalidateRect(state->window, nullptr, TRUE);
    return true;
}

void draw_window_shell(SettingsState* state) {
    auto* target = state->render_target.Get();
    target->FillRectangle(
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(kRailWidth), 720.0f),
        state->sidebar_bg_brush.Get());
    target->FillRectangle(
        D2D1::RectF(
            static_cast<float>(kRailWidth),
            0.0f,
            920.0f,
            static_cast<float>(kTitleBarHeight)),
        state->bg_brush.Get());
    target->DrawLine(
        D2D1::Point2F(static_cast<float>(kRailWidth), static_cast<float>(kTitleBarHeight)),
        D2D1::Point2F(920.0f, static_cast<float>(kTitleBarHeight)),
        state->border_brush.Get(),
        1.0f);

    target->DrawTextW(
        L"Air Screenshot",
        14,
        state->section_format.Get(),
        D2D1::RectF(20.0f, 5.0f, 190.0f, 30.0f),
        state->rail_text_brush.Get());
    target->DrawTextW(
        L"Capture Console",
        15,
        state->utility_format.Get(),
        D2D1::RectF(20.0f, 26.0f, 190.0f, 44.0f),
        state->rail_muted_brush.Get());

    const bool close_hovered =
        point_in_rect(state->mouse_pos, 872.0f, 0.0f, 920.0f, 48.0f);
    if (close_hovered) {
        target->FillRectangle(
            D2D1::RectF(872.0f, 0.0f, 920.0f, 48.0f),
            state->close_btn_hover_bg_brush.Get());
    }
    ID2D1SolidColorBrush* close_brush =
        close_hovered ? state->red_brush.Get() : state->text_grey_brush.Get();
    target->DrawLine(
        D2D1::Point2F(891.0f, 19.0f),
        D2D1::Point2F(901.0f, 29.0f),
        close_brush,
        1.5f);
    target->DrawLine(
        D2D1::Point2F(901.0f, 19.0f),
        D2D1::Point2F(891.0f, 29.0f),
        close_brush,
        1.5f);

    constexpr std::array<std::wstring_view, kCategoryCount> labels{
        L"截图与输出",
        L"文本与 OCR",
        L"工具栏",
        L"快捷键",
        L"应用与外观",
    };
    for (int index = 0; index < kCategoryCount; ++index) {
        const float top = static_cast<float>(kCategoryTop + index * kCategoryPitch);
        const bool selected = state->active_tab == index;
        const bool hovered =
            point_in_rect(state->mouse_pos, 12.0f, top, 212.0f, top + 40.0f);
        if (selected || hovered) {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(12.0f, top, 212.0f, top + 40.0f), 8.0f, 8.0f),
                selected ? state->active_tab_brush.Get()
                         : state->active_tab_brush.Get());
        }
        if (selected) {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(12.0f, top + 9.0f, 15.0f, top + 31.0f),
                    1.5f,
                    1.5f),
                state->accent_indicator_brush.Get());
        }
        ID2D1SolidColorBrush* brush =
            selected ? state->rail_text_brush.Get()
                     : (hovered ? state->rail_text_brush.Get()
                                : state->rail_muted_brush.Get());
        draw_nav_icon(state, index, 26.0f, top + 11.0f, brush);
        target->DrawTextW(
            labels[static_cast<std::size_t>(index)].data(),
            static_cast<UINT32>(labels[static_cast<std::size_t>(index)].size()),
            state->rail_format.Get(),
            D2D1::RectF(58.0f, top, 204.0f, top + 40.0f),
            brush);
    }

    const auto shortcut_card = D2D1::RoundedRect(
        D2D1::RectF(16.0f, 574.0f, 208.0f, 638.0f), 9.0f, 9.0f);
    target->FillRoundedRectangle(shortcut_card, state->active_tab_brush.Get());
    target->DrawTextW(
        L"截图快捷键",
        5,
        state->utility_format.Get(),
        D2D1::RectF(28.0f, 580.0f, 196.0f, 600.0f),
        state->rail_muted_brush.Get());
    const std::wstring& shortcut = state->config.capture_hotkey;
    const auto shortcut_rect = D2D1::RoundedRect(
        D2D1::RectF(28.0f, 602.0f, 196.0f, 628.0f), 6.0f, 6.0f);
    target->DrawRoundedRectangle(
        shortcut_rect, state->rail_muted_brush.Get(), 1.0f);
    target->DrawTextW(
        shortcut.c_str(),
        static_cast<UINT32>(shortcut.size()),
        state->hotkey_format.Get(),
        shortcut_rect.rect,
        state->rail_text_brush.Get());
}

void draw_workflow_strip(SettingsState* state) {
    auto* target = state->render_target.Get();
    const auto strip = D2D1::RoundedRect(
        D2D1::RectF(256.0f, 132.0f, 888.0f, 184.0f), 10.0f, 10.0f);
    target->FillRoundedRectangle(strip, state->card_bg_brush.Get());
    target->DrawRoundedRectangle(strip, state->card_border_brush.Get(), 1.0f);

    const bool output_file =
        _wcsicmp(state->config.default_output.c_str(), L"file") == 0;
    const std::array<std::wstring, 4> labels{
        L"截图 · " +
            (state->config.capture_hotkey.empty()
                 ? std::wstring(L"未设置")
                 : state->config.capture_hotkey),
        state->config.annotation_enabled ? L"标注 · 已启用" : L"标注 · 已关闭",
        state->config.ocr_enabled ? L"OCR · 按需" : L"OCR · 已关闭",
        output_file ? L"输出 · PNG" : L"输出 · 剪贴板",
    };
    constexpr std::array<float, 4> lefts{272.0f, 422.0f, 572.0f, 722.0f};
    for (std::size_t index = 0; index < lefts.size(); ++index) {
        const float left = lefts[index];
        const bool active =
            index == 0 ||
            (index == 1 && state->config.annotation_enabled) ||
            (index == 2 && state->config.ocr_enabled) ||
            index == 3;
        if (index + 1 < lefts.size()) {
            const float arrow_start = left + 136.0f;
            const float arrow_end = lefts[index + 1] - 5.0f;
            target->DrawLine(
                D2D1::Point2F(arrow_start, 158.0f),
                D2D1::Point2F(arrow_end, 158.0f),
                active ? state->blue_brush.Get() : state->separator_brush.Get(),
                1.0f);
            target->DrawLine(
                D2D1::Point2F(arrow_end - 4.0f, 154.0f),
                D2D1::Point2F(arrow_end, 158.0f),
                active ? state->blue_brush.Get() : state->separator_brush.Get(),
                1.0f);
            target->DrawLine(
                D2D1::Point2F(arrow_end - 4.0f, 162.0f),
                D2D1::Point2F(arrow_end, 158.0f),
                active ? state->blue_brush.Get() : state->separator_brush.Get(),
                1.0f);
        }
        ID2D1SolidColorBrush* icon_brush =
            active ? (index == 3 ? state->cyan_brush.Get()
                                 : state->blue_brush.Get())
                   : state->text_grey_brush.Get();
        const float icon_x = left + 7.0f;
        if (index == 0) {
            target->DrawLine(
                D2D1::Point2F(icon_x, 151.0f),
                D2D1::Point2F(icon_x, 156.0f),
                icon_brush,
                1.5f);
            target->DrawLine(
                D2D1::Point2F(icon_x, 151.0f),
                D2D1::Point2F(icon_x + 5.0f, 151.0f),
                icon_brush,
                1.5f);
            target->DrawLine(
                D2D1::Point2F(icon_x + 14.0f, 160.0f),
                D2D1::Point2F(icon_x + 14.0f, 165.0f),
                icon_brush,
                1.5f);
            target->DrawLine(
                D2D1::Point2F(icon_x + 9.0f, 165.0f),
                D2D1::Point2F(icon_x + 14.0f, 165.0f),
                icon_brush,
                1.5f);
        } else if (index == 1) {
            target->DrawLine(
                D2D1::Point2F(icon_x + 2.0f, 164.0f),
                D2D1::Point2F(icon_x + 13.0f, 153.0f),
                icon_brush,
                1.8f);
            target->DrawLine(
                D2D1::Point2F(icon_x + 1.0f, 165.0f),
                D2D1::Point2F(icon_x + 5.0f, 164.0f),
                icon_brush,
                1.2f);
        } else if (index == 2) {
            target->DrawLine(
                D2D1::Point2F(icon_x + 1.0f, 152.0f),
                D2D1::Point2F(icon_x + 13.0f, 152.0f),
                icon_brush,
                1.5f);
            target->DrawLine(
                D2D1::Point2F(icon_x + 7.0f, 152.0f),
                D2D1::Point2F(icon_x + 7.0f, 165.0f),
                icon_brush,
                1.5f);
        } else {
            target->DrawRectangle(
                D2D1::RectF(icon_x + 1.0f, 151.0f, icon_x + 11.0f, 163.0f),
                icon_brush,
                1.3f);
            target->DrawRectangle(
                D2D1::RectF(icon_x + 5.0f, 154.0f, icon_x + 15.0f, 166.0f),
                icon_brush,
                1.3f);
        }
        target->DrawTextW(
            labels[index].c_str(),
            static_cast<UINT32>(labels[index].size()),
            state->utility_format.Get(),
            D2D1::RectF(left + 28.0f, 136.0f, left + 134.0f, 180.0f),
            active ? state->text_white_brush.Get()
                   : state->text_grey_brush.Get());
    }
}

void draw_capture_page(SettingsState* state) {
    draw_page_header(
        state, L"截图与输出", L"决定截图完成后的默认流程与结果。");
    draw_workflow_strip(state);

    draw_section_title(state, L"截图行为", 198.0f);
    draw_row_group(state, 226.0f, 338.0f);
    const bool annotation_hovered =
        point_in_rect(state->mouse_pos, 256.0f, 226.0f, 888.0f, 282.0f);
    draw_setting_row(
        state,
        226.0f,
        282.0f,
        L"启用截图标注",
        L"截图后显示轻量编辑工具。",
        annotation_hovered);
    draw_switch(
        state,
        828,
        242,
        state->config.annotation_enabled,
        true,
        annotation_hovered);
    const bool locked_enabled = state->config.annotation_enabled;
    const bool locked_hovered =
        locked_enabled &&
        point_in_rect(state->mouse_pos, 256.0f, 282.0f, 888.0f, 338.0f);
    draw_setting_row(
        state,
        282.0f,
        338.0f,
        L"连续使用当前工具",
        locked_enabled ? L"完成一次标注后保留当前工具。"
                       : L"启用截图标注后可设置。",
        locked_hovered,
        locked_enabled,
        false);
    draw_switch(
        state,
        828,
        298,
        state->config.annotation_locked_tool,
        locked_enabled,
        locked_hovered);

    draw_section_title(state, L"默认结果", 386.0f);
    draw_row_group(state, 414.0f, 590.0f);
    draw_setting_row(
        state,
        414.0f,
        478.0f,
        L"截图完成后",
        L"未手动选择操作时使用。",
        false);
    const bool default_file =
        _wcsicmp(state->config.default_output.c_str(), L"file") == 0;
    draw_choice_button(
        state,
        594,
        429,
        724,
        465,
        L"复制到剪贴板",
        !default_file,
        point_in_rect(state->mouse_pos, 594.0f, 429.0f, 724.0f, 465.0f));
    draw_choice_button(
        state,
        732,
        429,
        870,
        465,
        L"保存为 PNG",
        default_file,
        point_in_rect(state->mouse_pos, 732.0f, 429.0f, 870.0f, 465.0f));

    const bool notification_hovered =
        point_in_rect(state->mouse_pos, 256.0f, 478.0f, 888.0f, 534.0f);
    draw_setting_row(
        state,
        478.0f,
        534.0f,
        L"完成时显示通知",
        L"仅在后台操作完成后提醒。",
        notification_hovered);
    draw_switch(
        state,
        828,
        494,
        state->config.notifications_enabled,
        true,
        notification_hovered);

    const bool reset_hovered =
        point_in_rect(state->mouse_pos, 766.0f, 544.0f, 870.0f, 580.0f);
    const std::wstring serial_helper =
        std::format(L"下一次标注从 {} 开始。", state->config.annotation_next_serial);
    draw_setting_row(
        state,
        534.0f,
        590.0f,
        L"序号标注",
        serial_helper,
        false,
        true,
        false);
    draw_button(
        state,
        766,
        544,
        870,
        580,
        state->config.annotation_next_serial == 1 ? L"已重置" : L"重置为 1",
        reset_hovered,
        btn_secondary,
        state->config.annotation_next_serial != 1);
}

void draw_ocr_page(SettingsState* state) {
    draw_page_header(
        state, L"文本与 OCR", L"管理本地识别模型与标注文字样式。");
    draw_section_title(state, L"文字识别", 126.0f);
    draw_row_group(state, 150.0f, 350.0f);
    const bool ocr_hovered =
        point_in_rect(state->mouse_pos, 256.0f, 150.0f, 888.0f, 206.0f);
    draw_setting_row(
        state,
        150.0f,
        206.0f,
        L"启用 OCR 识别",
        L"在截图界面中提供屏幕文字识别。",
        ocr_hovered);
    draw_switch(
        state, 828, 166, state->config.ocr_enabled, true, ocr_hovered);

    draw_setting_row(
        state,
        206.0f,
        278.0f,
        L"默认识别引擎",
        state->config.ocr_enabled ? L"选择速度、精度与兼容性的平衡。"
                                  : L"启用 OCR 后可选择。",
        false,
        state->config.ocr_enabled);
    const std::wstring current_engine =
        normalize_ocr_engine(state->config.ocr_engine);
    for (const auto& button : kOcrEngineButtons) {
        const bool selected = current_engine == button.engine;
        const bool hovered =
            state->config.ocr_enabled && !state->is_downloading &&
            point_in_rect(
                state->mouse_pos,
                static_cast<float>(button.left),
                222.0f,
                static_cast<float>(button.right),
                258.0f);
        draw_choice_button(
            state,
            button.left,
            222,
            button.right,
            258,
            button.label,
            selected,
            hovered);
    }

    std::wstring dependency_status = state->ocr_dependency.message;
    if (state->is_downloading) {
        dependency_status =
            std::format(L"正在下载并校验本地模型 · {}%", state->download_progress);
    } else if (!state->download_error.empty()) {
        dependency_status = state->download_error;
    }
    draw_setting_row(
        state,
        278.0f,
        350.0f,
        L"本地 OCR 依赖",
        dependency_status,
        false,
        state->config.ocr_enabled,
        false);
    if (state->is_downloading) {
        const float progress =
            static_cast<float>(std::clamp(state->download_progress, 0, 100)) / 100.0f;
        const auto track = D2D1::RoundedRect(
            D2D1::RectF(690.0f, 306.0f, 870.0f, 314.0f), 4.0f, 4.0f);
        state->render_target->FillRoundedRectangle(
            track, state->disabled_bg_brush.Get());
        state->render_target->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(690.0f, 306.0f, 690.0f + 180.0f * progress, 314.0f),
                4.0f,
                4.0f),
            state->blue_brush.Get());
    } else if (state->config.ocr_enabled && state->ocr_dependency.can_download) {
        const bool hovered =
            point_in_rect(state->mouse_pos, 746.0f, 294.0f, 870.0f, 330.0f);
        draw_button(
            state,
            746,
            294,
            870,
            330,
            state->download_error.empty() ? L"下载依赖" : L"重新下载",
            hovered,
            btn_primary);
    } else if (state->config.ocr_enabled && state->ocr_dependency.ready) {
        draw_choice_button(
            state, 786, 294, 870, 330, L"已就绪", true, false);
    } else if (state->config.ocr_enabled) {
        draw_button(
            state,
            786,
            294,
            870,
            330,
            L"不可用",
            false,
            btn_secondary,
            false);
    }

    draw_section_title(state, L"标注文字", 392.0f);
    draw_row_group(state, 418.0f, 530.0f);
    draw_setting_row(
        state,
        418.0f,
        474.0f,
        L"字体",
        L"用于截图中的文本标注。",
        false);
    const std::wstring font_label = state->config.text_font_family;
    draw_button(
        state,
        630,
        428,
        870,
        464,
        font_label.c_str(),
        point_in_rect(state->mouse_pos, 630.0f, 428.0f, 870.0f, 464.0f),
        btn_secondary);
    draw_setting_row(
        state,
        474.0f,
        530.0f,
        L"字形",
        L"新建文字标注时使用。",
        false,
        true,
        false);
    draw_choice_button(
        state,
        736,
        484,
        796,
        520,
        L"加粗",
        state->config.text_font_bold,
        point_in_rect(state->mouse_pos, 736.0f, 484.0f, 796.0f, 520.0f));
    draw_choice_button(
        state,
        808,
        484,
        870,
        520,
        L"倾斜",
        state->config.text_font_italic,
        point_in_rect(state->mouse_pos, 808.0f, 484.0f, 870.0f, 520.0f));
}

void draw_toolbar_page(SettingsState* state) {
    draw_page_header(
        state, L"工具栏", L"拖动改变顺序；隐藏的工具仍可通过快捷键使用。");
    auto tools = split_by_comma(state->config.toolbar_order);
    const int max_offset =
        std::max(0, static_cast<int>(tools.size()) - kToolbarVisibleRows);
    state->toolbar_scroll_offset =
        std::clamp(state->toolbar_scroll_offset, 0, max_offset);

    state->render_target->DrawTextW(
        L"工具顺序",
        4,
        state->section_format.Get(),
        D2D1::RectF(256.0f, 126.0f, 648.0f, 152.0f),
        state->text_white_brush.Get());
    state->render_target->DrawTextW(
        L"拖动手柄排序",
        6,
        state->utility_format.Get(),
        D2D1::RectF(524.0f, 126.0f, 648.0f, 152.0f),
        state->text_grey_brush.Get());
    const auto list_rect = D2D1::RoundedRect(
        D2D1::RectF(256.0f, 154.0f, 650.0f, 608.0f), 10.0f, 10.0f);
    state->render_target->FillRoundedRectangle(
        list_rect, state->card_bg_brush.Get());
    state->render_target->DrawRoundedRectangle(
        list_rect, state->card_border_brush.Get(), 1.0f);

    for (int visible = 0; visible < kToolbarVisibleRows; ++visible) {
        const int index = state->toolbar_scroll_offset + visible;
        if (index >= static_cast<int>(tools.size())) {
            break;
        }
        const float top = 160.0f + visible * kToolbarRowHeight;
        const bool selected = state->selected_tool_idx == index;
        const bool hovered =
            point_in_rect(state->mouse_pos, 258.0f, top, 646.0f, top + 38.0f);
        if (selected || hovered) {
            state->render_target->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(260.0f, top + 2.0f, 642.0f, top + 38.0f),
                    6.0f,
                    6.0f),
                selected ? state->accent_soft_brush.Get()
                         : state->hover_bg_brush.Get());
        }
        for (int dot = 0; dot < 6; ++dot) {
            const int column = dot % 2;
            const int row = dot / 2;
            state->render_target->FillEllipse(
                D2D1::Ellipse(
                    D2D1::Point2F(
                        274.0f + column * 6.0f,
                        top + 14.0f + row * 5.0f),
                    1.2f,
                    1.2f),
                selected ? state->blue_brush.Get() : state->text_grey_brush.Get());
        }
        const bool shown =
            !annotation_tool_hidden(state->config.annotation_hidden_tools, tools[index]);
        const auto checkbox = D2D1::RoundedRect(
            D2D1::RectF(300.0f, top + 11.0f, 318.0f, top + 29.0f), 4.0f, 4.0f);
        if (shown) {
            state->render_target->FillRoundedRectangle(
                checkbox, state->blue_brush.Get());
            state->render_target->DrawLine(
                D2D1::Point2F(305.0f, top + 20.0f),
                D2D1::Point2F(309.0f, top + 24.0f),
                state->accent_text_brush.Get(),
                1.5f);
            state->render_target->DrawLine(
                D2D1::Point2F(309.0f, top + 24.0f),
                D2D1::Point2F(315.0f, top + 16.0f),
                state->accent_text_brush.Get(),
                1.5f);
        } else {
            state->render_target->DrawRoundedRectangle(
                checkbox, state->text_grey_brush.Get(), 1.0f);
        }
        const std::wstring name = get_tool_display_name(tools[index]);
        state->render_target->DrawTextW(
            name.c_str(),
            static_cast<UINT32>(name.size()),
            state->text_format.Get(),
            D2D1::RectF(332.0f, top, 548.0f, top + 40.0f),
            shown ? state->text_white_brush.Get() : state->text_grey_brush.Get());
        state->render_target->DrawTextW(
            shown ? L"显示" : L"隐藏",
            2,
            state->utility_format.Get(),
            D2D1::RectF(574.0f, top, 626.0f, top + 40.0f),
            shown ? state->blue_brush.Get() : state->text_grey_brush.Get());
        if (visible + 1 < kToolbarVisibleRows &&
            index + 1 < static_cast<int>(tools.size())) {
            state->render_target->DrawLine(
                D2D1::Point2F(300.0f, top + 40.0f),
                D2D1::Point2F(630.0f, top + 40.0f),
                state->separator_brush.Get(),
                1.0f);
        }
    }
    if (state->drag_target_idx >= 0) {
        const int visible = state->drag_target_idx - state->toolbar_scroll_offset;
        if (visible >= 0 && visible < kToolbarVisibleRows) {
            const float top = 160.0f + visible * kToolbarRowHeight;
            state->render_target->DrawLine(
                D2D1::Point2F(268.0f, top),
                D2D1::Point2F(638.0f, top),
                state->blue_brush.Get(),
                2.0f);
        }
    }
    if (max_offset > 0) {
        const float track_top = 166.0f;
        const float track_height = 426.0f;
        const float thumb_height =
            std::max(48.0f, track_height * kToolbarVisibleRows /
                                 static_cast<float>(tools.size()));
        const float thumb_top =
            track_top + (track_height - thumb_height) *
                            state->toolbar_scroll_offset /
                            static_cast<float>(max_offset);
        state->render_target->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(642.0f, track_top, 646.0f, track_top + track_height),
                2.0f,
                2.0f),
            state->disabled_bg_brush.Get());
        state->render_target->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(642.0f, thumb_top, 646.0f, thumb_top + thumb_height),
                2.0f,
                2.0f),
            state->switch_track_off_brush.Get());
    }

    state->render_target->DrawTextW(
        L"所选工具",
        4,
        state->section_format.Get(),
        D2D1::RectF(672.0f, 126.0f, 888.0f, 152.0f),
        state->text_white_brush.Get());
    const auto inspector = D2D1::RoundedRect(
        D2D1::RectF(672.0f, 154.0f, 888.0f, 332.0f), 10.0f, 10.0f);
    state->render_target->FillRoundedRectangle(
        inspector, state->card_bg_brush.Get());
    state->render_target->DrawRoundedRectangle(
        inspector, state->card_border_brush.Get(), 1.0f);
    if (state->selected_tool_idx >= 0 &&
        state->selected_tool_idx < static_cast<int>(tools.size())) {
        const std::wstring selected =
            get_tool_display_name(tools[state->selected_tool_idx]);
        state->render_target->DrawTextW(
            selected.c_str(),
            static_cast<UINT32>(selected.size()),
            state->section_format.Get(),
            D2D1::RectF(692.0f, 164.0f, 868.0f, 194.0f),
            state->text_white_brush.Get());
        const bool shown = !annotation_tool_hidden(
            state->config.annotation_hidden_tools,
            tools[state->selected_tool_idx]);
        state->render_target->DrawTextW(
            L"在工具栏中显示",
            7,
            state->text_format.Get(),
            D2D1::RectF(692.0f, 198.0f, 812.0f, 246.0f),
            state->text_white_brush.Get());
        const bool visibility_hovered =
            point_in_rect(state->mouse_pos, 674.0f, 190.0f, 884.0f, 246.0f);
        draw_switch(
            state, 824, 206, shown, true, visibility_hovered);
        const bool can_move_up = state->selected_tool_idx > 0;
        const bool can_move_down =
            state->selected_tool_idx + 1 < static_cast<int>(tools.size());
        draw_button(
            state,
            674,
            274,
            776,
            312,
            L"上移",
            point_in_rect(state->mouse_pos, 674.0f, 274.0f, 776.0f, 312.0f),
            btn_secondary,
            can_move_up);
        draw_button(
            state,
            784,
            274,
            886,
            312,
            L"下移",
            point_in_rect(state->mouse_pos, 784.0f, 274.0f, 886.0f, 312.0f),
            btn_secondary,
            can_move_down);
    } else {
        state->render_target->DrawTextW(
            L"选择一个工具以调整显示状态和顺序。",
            16,
            state->small_format.Get(),
            D2D1::RectF(692.0f, 174.0f, 868.0f, 228.0f),
            state->text_grey_brush.Get());
    }
    constexpr std::wstring_view drag_tip = L"拖动左侧手柄可快速排序。";
    constexpr std::wstring_view keyboard_tip = L"键盘：Alt + ↑ / ↓";
    state->render_target->DrawTextW(
        drag_tip.data(),
        static_cast<UINT32>(drag_tip.size()),
        state->small_format.Get(),
        D2D1::RectF(672.0f, 350.0f, 888.0f, 382.0f),
        state->text_grey_brush.Get());
    state->render_target->DrawTextW(
        keyboard_tip.data(),
        static_cast<UINT32>(keyboard_tip.size()),
        state->small_format.Get(),
        D2D1::RectF(672.0f, 376.0f, 888.0f, 408.0f),
        state->text_grey_brush.Get());
}

void draw_shortcuts_page(SettingsState* state) {
    draw_page_header(
        state, L"快捷键", L"点击按键框后直接输入；Delete 清除，Esc 取消。");
    state->shortcut_error = shortcut_validation_error(state->config);
    const bool has_error = state->shortcut_error.has_value();
    state->render_target->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(262.0f, 141.0f), 3.5f, 3.5f),
        has_error ? state->red_brush.Get()
                  : (state->capturing_idx_ >= 0 ? state->blue_brush.Get()
                                                : state->cyan_brush.Get()));
    const std::wstring status =
        has_error
            ? *state->shortcut_error
            : (state->capturing_idx_ >= 0
                   ? L"正在记录新的快捷键。按 Esc 取消。"
                   : L"快捷键会在保存时统一校验并立即生效。");
    state->render_target->DrawTextW(
        status.c_str(),
        static_cast<UINT32>(status.size()),
        state->small_format.Get(),
        D2D1::RectF(274.0f, 126.0f, 888.0f, 158.0f),
        has_error ? state->red_brush.Get() : state->text_grey_brush.Get());

    draw_section_title(state, L"全局操作", 164.0f);
    draw_row_group(state, 192.0f, 400.0f);
    const bool global_enabled = state->config.ocr_enabled;
    const bool global_hovered =
        global_enabled &&
        point_in_rect(state->mouse_pos, 256.0f, 192.0f, 888.0f, 244.0f);
    draw_setting_row(
        state,
        192.0f,
        244.0f,
        L"启用全局 OCR 快捷键",
        global_enabled ? L"无需先打开截图界面即可识别。"
                       : L"先在“文本与 OCR”中启用识别。",
        global_hovered,
        global_enabled);
    draw_switch(
        state,
        828,
        206,
        state->config.global_ocr_enabled,
        global_enabled,
        global_hovered);
    constexpr std::array<std::wstring_view, 3> labels{
        L"截图",
        L"全局 OCR",
        L"选区 OCR",
    };
    for (int index = 0; index < 3; ++index) {
        const float top = 244.0f + index * 52.0f;
        const std::wstring* value = get_shortcut_ptr(state->config, index);
        state->render_target->DrawTextW(
            labels[static_cast<std::size_t>(index)].data(),
            static_cast<UINT32>(labels[static_cast<std::size_t>(index)].size()),
            state->text_format.Get(),
            D2D1::RectF(276.0f, top, 628.0f, top + 52.0f),
            state->text_white_brush.Get());
        draw_hotkey_box(
            state,
            684,
            static_cast<int>(top + 8.0f),
            870,
            static_cast<int>(top + 44.0f),
            value ? value->c_str() : L"",
            state->capturing_idx_ == index,
            point_in_rect(
                state->mouse_pos,
                684.0f,
                top + 8.0f,
                870.0f,
                top + 44.0f));
        if (index < 2) {
            state->render_target->DrawLine(
                D2D1::Point2F(276.0f, top + 52.0f),
                D2D1::Point2F(868.0f, top + 52.0f),
                state->separator_brush.Get(),
                1.0f);
        }
    }

    draw_section_title(state, L"标注工具", 404.0f);
    const auto tools_group = D2D1::RoundedRect(
        D2D1::RectF(256.0f, 432.0f, 888.0f, 652.0f), 10.0f, 10.0f);
    state->render_target->FillRoundedRectangle(
        tools_group, state->card_bg_brush.Get());
    state->render_target->DrawRoundedRectangle(
        tools_group, state->card_border_brush.Get(), 1.0f);
    for (int index = 3; index < shortcut_count; ++index) {
        const int tool_index = index - 3;
        const int column = tool_index / 6;
        const int row = tool_index % 6;
        const float column_left = column == 0 ? 276.0f : 592.0f;
        const float top = 436.0f + row * 36.0f;
        const std::wstring* value = get_shortcut_ptr(state->config, index);
        const std::wstring_view label =
            kShortcutLabels[static_cast<std::size_t>(index)];
        state->render_target->DrawTextW(
            label.data(),
            static_cast<UINT32>(label.size()),
            state->utility_format.Get(),
            D2D1::RectF(column_left, top, column_left + 140.0f, top + 36.0f),
            state->text_white_brush.Get());
        const int field_left = column == 0 ? 420 : 736;
        draw_hotkey_box(
            state,
            field_left,
            static_cast<int>(top + 3.0f),
            field_left + 134,
            static_cast<int>(top + 33.0f),
            value ? value->c_str() : L"",
            state->capturing_idx_ == index,
            point_in_rect(
                state->mouse_pos,
                static_cast<float>(field_left),
                top + 3.0f,
                static_cast<float>(field_left + 134),
                top + 33.0f));
    }
}

void draw_app_page(SettingsState* state) {
    draw_page_header(
        state, L"应用与外观", L"控制后台运行方式、窗口主题与应用图标。");
    draw_section_title(state, L"后台运行", 126.0f);
    draw_row_group(state, 152.0f, 264.0f);
    const bool shell_hovered =
        point_in_rect(state->mouse_pos, 256.0f, 152.0f, 888.0f, 208.0f);
    draw_setting_row(
        state,
        152.0f,
        208.0f,
        L"运行托盘与全局快捷键",
        L"关闭后仅保留命令行与当前窗口操作。",
        shell_hovered);
    draw_switch(
        state, 828, 168, state->config.shell_enabled, true, shell_hovered);
    const bool startup_enabled = state->config.shell_enabled;
    const bool startup_hovered =
        startup_enabled &&
        point_in_rect(state->mouse_pos, 256.0f, 208.0f, 888.0f, 264.0f);
    draw_setting_row(
        state,
        208.0f,
        264.0f,
        L"登录 Windows 后自动启动",
        startup_enabled ? L"在登录后静默启动截图服务。"
                        : L"启用后台运行后可设置。",
        startup_hovered,
        startup_enabled,
        false);
    draw_switch(
        state,
        828,
        224,
        state->config.start_at_login,
        startup_enabled,
        startup_hovered);

    draw_section_title(state, L"外观", 302.0f);
    draw_row_group(state, 328.0f, 398.0f);
    draw_setting_row(
        state,
        328.0f,
        398.0f,
        L"应用主题",
        L"更改会立即预览，保存后生效。",
        false,
        true,
        false);
    struct ThemeChoice {
        std::wstring_view value;
        const wchar_t* label;
        int left;
        int right;
    };
    constexpr std::array<ThemeChoice, 3> choices{{
        {L"system", L"跟随系统", 486, 608},
        {L"light", L"浅色", 616, 738},
        {L"dark", L"深色", 746, 870},
    }};
    for (const auto& choice : choices) {
        draw_choice_button(
            state,
            choice.left,
            344,
            choice.right,
            382,
            choice.label,
            state->config.theme == choice.value,
            point_in_rect(
                state->mouse_pos,
                static_cast<float>(choice.left),
                344.0f,
                static_cast<float>(choice.right),
                382.0f));
    }

    draw_section_title(state, L"应用图标", 430.0f);
    draw_row_group(state, 456.0f, 612.0f);
    struct AppIconChoice {
        std::wstring_view value;
        const wchar_t* label;
        const wchar_t* helper;
        int left;
        int right;
    };
    constexpr std::array<AppIconChoice, 3> icon_choices{{
        {kAppIconFocusFrame, L"精准取景", L"角框定位 · 默认", 276, 468},
        {kAppIconFlowLens, L"流光镜", L"连续捕捉 · 动态", 476, 668},
        {kAppIconPixelConsole, L"像素舱", L"模块输出 · 技术", 676, 868},
    }};
    for (int index = 0; index < static_cast<int>(icon_choices.size()); ++index) {
        const auto& choice = icon_choices[static_cast<std::size_t>(index)];
        const bool selected = state->config.app_icon == choice.value;
        const bool hovered = point_in_rect(
            state->mouse_pos,
            static_cast<float>(choice.left),
            470.0f,
            static_cast<float>(choice.right),
            552.0f);
        const auto rect = D2D1::RoundedRect(
            D2D1::RectF(
                static_cast<float>(choice.left),
                470.0f,
                static_cast<float>(choice.right),
                552.0f),
            8.0f,
            8.0f);
        state->render_target->FillRoundedRectangle(
            rect,
            selected ? state->accent_soft_brush.Get()
                     : (hovered ? state->hover_bg_brush.Get()
                                : state->control_bg_brush.Get()));
        state->render_target->DrawRoundedRectangle(
            rect,
            selected ? state->hover_blue_brush.Get()
                     : state->card_border_brush.Get(),
            selected ? 1.5f : 1.0f);

        const float icon_x = static_cast<float>(choice.left + 14);
        constexpr float icon_y = 489.0f;
        constexpr float icon_size = 40.0f;
        auto* target = state->render_target.Get();
        if (index == 0) {
            const float margin = icon_size * 0.125f;
            target->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(
                        icon_x + margin,
                        icon_y + margin,
                        icon_x + icon_size - margin,
                        icon_y + icon_size - margin),
                    icon_size * 0.15625f,
                    icon_size * 0.15625f),
                state->sidebar_bg_brush.Get());
            const float p0 = icon_size * 0.28125f;
            const float p1 = icon_size * 0.4375f;
            const float p2 = icon_size * 0.5625f;
            const float p3 = icon_size * 0.71875f;
            const float stroke = 2.1f;
            target->DrawLine(D2D1::Point2F(icon_x + p0, icon_y + p1), D2D1::Point2F(icon_x + p0, icon_y + p0), state->blue_brush.Get(), stroke);
            target->DrawLine(D2D1::Point2F(icon_x + p0, icon_y + p0), D2D1::Point2F(icon_x + p1, icon_y + p0), state->blue_brush.Get(), stroke);
            target->DrawLine(D2D1::Point2F(icon_x + p2, icon_y + p0), D2D1::Point2F(icon_x + p3, icon_y + p0), state->blue_brush.Get(), stroke);
            target->DrawLine(D2D1::Point2F(icon_x + p3, icon_y + p0), D2D1::Point2F(icon_x + p3, icon_y + p1), state->blue_brush.Get(), stroke);
            target->DrawLine(D2D1::Point2F(icon_x + p0, icon_y + p2), D2D1::Point2F(icon_x + p0, icon_y + p3), state->blue_brush.Get(), stroke);
            target->DrawLine(D2D1::Point2F(icon_x + p0, icon_y + p3), D2D1::Point2F(icon_x + p1, icon_y + p3), state->blue_brush.Get(), stroke);
            target->DrawLine(D2D1::Point2F(icon_x + p2, icon_y + p3), D2D1::Point2F(icon_x + p3, icon_y + p3), state->blue_brush.Get(), stroke);
            target->DrawLine(D2D1::Point2F(icon_x + p3, icon_y + p2), D2D1::Point2F(icon_x + p3, icon_y + p3), state->blue_brush.Get(), stroke);
            const float focus = icon_size * 0.0703125f;
            target->FillRectangle(
                D2D1::RectF(
                    icon_x + (icon_size - focus) * 0.5f,
                    icon_y + (icon_size - focus) * 0.5f,
                    icon_x + (icon_size + focus) * 0.5f,
                    icon_y + (icon_size + focus) * 0.5f),
                state->cyan_brush.Get());
        } else if (index == 1) {
            const float margin = icon_size * 0.125f;
            target->FillEllipse(
                D2D1::Ellipse(
                    D2D1::Point2F(icon_x + icon_size * 0.5f, icon_y + icon_size * 0.5f),
                    icon_size * 0.375f,
                    icon_size * 0.375f),
                state->sidebar_bg_brush.Get());
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
            if (SUCCEEDED(state->d2d_factory->CreatePathGeometry(geometry.GetAddressOf()))) {
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(geometry->Open(sink.GetAddressOf()))) {
                    sink->BeginFigure(
                        D2D1::Point2F(icon_x + icon_size * 0.5f, icon_y + margin),
                        D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddArc(D2D1::ArcSegment(
                        D2D1::Point2F(
                            icon_x + icon_size * 0.875f,
                            icon_y + icon_size * 0.5f),
                        D2D1::SizeF(icon_size * 0.375f, icon_size * 0.375f),
                        0.0f,
                        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                        D2D1_ARC_SIZE_LARGE));
                    sink->AddLine(D2D1::Point2F(
                        icon_x + icon_size * 0.75f,
                        icon_y + icon_size * 0.5f));
                    sink->AddArc(D2D1::ArcSegment(
                        D2D1::Point2F(
                            icon_x + icon_size * 0.5f,
                            icon_y + icon_size * 0.25f),
                        D2D1::SizeF(icon_size * 0.25f, icon_size * 0.25f),
                        0.0f,
                        D2D1_SWEEP_DIRECTION_CLOCKWISE,
                        D2D1_ARC_SIZE_LARGE));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    if (SUCCEEDED(sink->Close())) {
                        target->FillGeometry(geometry.Get(), state->blue_brush.Get());
                    }
                }
            }
            target->FillRectangle(
                D2D1::RectF(
                    icon_x + icon_size * 0.5f,
                    icon_y + icon_size * 0.47265625f,
                    icon_x + icon_size * 0.75f,
                    icon_y + icon_size * 0.52734375f),
                state->blue_brush.Get());
            const float locator = std::max(2.0f, icon_size * 0.0625f);
            target->FillRectangle(
                D2D1::RectF(
                    icon_x + icon_size * 0.71875f,
                    icon_y + icon_size * 0.484375f,
                    icon_x + icon_size * 0.71875f + locator,
                    icon_y + icon_size * 0.484375f + locator),
                state->cyan_brush.Get());
        } else {
            const float cell = icon_size * 0.1953125f;
            const float gap = icon_size * 0.01953125f;
            const float origin = icon_size * 0.1875f;
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    ID2D1SolidColorBrush* brush =
                        row == 1 && column == 1
                            ? state->cyan_brush.Get()
                            : ((row + column) % 2 == 1
                                   ? state->blue_brush.Get()
                                   : state->sidebar_bg_brush.Get());
                    const float left = icon_x + origin + column * (cell + gap);
                    const float top = icon_y + origin + row * (cell + gap);
                    target->FillRectangle(
                        D2D1::RectF(left, top, left + cell, top + cell), brush);
                }
            }
        }

        state->render_target->DrawTextW(
            choice.label,
            static_cast<UINT32>(wcslen(choice.label)),
            state->text_format.Get(),
            D2D1::RectF(
                static_cast<float>(choice.left + 66),
                481.0f,
                static_cast<float>(choice.right - 22),
                507.0f),
            selected ? state->hover_blue_brush.Get()
                     : state->text_white_brush.Get());
        state->render_target->DrawTextW(
            choice.helper,
            static_cast<UINT32>(wcslen(choice.helper)),
            state->small_format.Get(),
            D2D1::RectF(
                static_cast<float>(choice.left + 66),
                509.0f,
                static_cast<float>(choice.right - 12),
                536.0f),
            state->text_grey_brush.Get());
        if (selected) {
            const D2D1_POINT_2F center = D2D1::Point2F(
                static_cast<float>(choice.right - 14), 484.0f);
            state->render_target->FillEllipse(
                D2D1::Ellipse(center, 6.0f, 6.0f), state->blue_brush.Get());
            state->render_target->DrawLine(
                D2D1::Point2F(center.x - 2.5f, center.y),
                D2D1::Point2F(center.x - 0.5f, center.y + 2.0f),
                state->accent_text_brush.Get(),
                1.3f);
            state->render_target->DrawLine(
                D2D1::Point2F(center.x - 0.5f, center.y + 2.0f),
                D2D1::Point2F(center.x + 3.0f, center.y - 2.5f),
                state->accent_text_brush.Get(),
                1.3f);
        }
    }
    constexpr std::wstring_view icon_helper =
        L"同步托盘、任务栏与运行中的窗口；程序文件图标保持默认款。";
    state->render_target->DrawTextW(
        icon_helper.data(),
        static_cast<UINT32>(icon_helper.size()),
        state->small_format.Get(),
        D2D1::RectF(276.0f, 568.0f, 868.0f, 598.0f),
        state->text_grey_brush.Get());
}

void draw_footer(SettingsState* state) {
    auto* target = state->render_target.Get();
    target->FillRectangle(
        D2D1::RectF(
            static_cast<float>(kRailWidth),
            static_cast<float>(kFooterTop),
            920.0f,
            720.0f),
        state->card_bg_brush.Get());
    target->DrawLine(
        D2D1::Point2F(static_cast<float>(kRailWidth), static_cast<float>(kFooterTop)),
        D2D1::Point2F(920.0f, static_cast<float>(kFooterTop)),
        state->border_brush.Get(),
        1.0f);
    const bool dirty = settings_are_dirty(state);
    const bool invalid = state->shortcut_error.has_value();
    ID2D1SolidColorBrush* status_brush =
        invalid ? state->red_brush.Get()
                : (dirty ? state->blue_brush.Get() : state->cyan_brush.Get());
    target->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(260.0f, 691.0f), 3.5f, 3.5f),
        status_brush);
    const wchar_t* status =
        invalid ? L"请先修复快捷键冲突"
                : (dirty ? L"有未保存的更改" : L"设置已同步");
    target->DrawTextW(
        status,
        static_cast<UINT32>(wcslen(status)),
        state->small_format.Get(),
        D2D1::RectF(272.0f, 670.0f, 620.0f, 712.0f),
        invalid ? state->red_brush.Get() : state->text_grey_brush.Get());
    draw_button(
        state,
        672,
        670,
        764,
        710,
        L"取消",
        point_in_rect(state->mouse_pos, 672.0f, 670.0f, 764.0f, 710.0f),
        btn_secondary);
    draw_button(
        state,
        776,
        670,
        888,
        710,
        L"保存更改",
        point_in_rect(state->mouse_pos, 776.0f, 670.0f, 888.0f, 710.0f),
        btn_primary,
        dirty && !invalid);
}

void paint_settings(SettingsState* state) {
    auto* target = state->render_target.Get();
    target->Clear(
        state->high_contrast
            ? color_from_system(COLOR_WINDOW)
            : (state->is_light_theme ? D2D1::ColorF(0xF6F8FC)
                                     : D2D1::ColorF(0x0F141D)));
    draw_window_shell(state);
    switch (state->active_tab) {
        case 0: draw_capture_page(state); break;
        case 1: draw_ocr_page(state); break;
        case 2: draw_toolbar_page(state); break;
        case 3: draw_shortcuts_page(state); break;
        case 4: draw_app_page(state); break;
        default: break;
    }
    draw_footer(state);
    draw_settings_keyboard_focus(state);
}

POINT settings_point_from_client(HWND window, LPARAM l_param) {
    POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    const float scale = settings_layout_scale(window);
    point.x = static_cast<LONG>(std::lround(point.x / scale));
    point.y = static_cast<LONG>(std::lround(point.y / scale));
    return point;
}

POINT settings_point_from_cursor(HWND window) {
    POINT point{};
    GetCursorPos(&point);
    ScreenToClient(window, &point);
    const float scale = settings_layout_scale(window);
    point.x = static_cast<LONG>(std::lround(point.x / scale));
    point.y = static_cast<LONG>(std::lround(point.y / scale));
    return point;
}

int toolbar_index_at_point(const SettingsState* state, POINT point) {
    if (!point_in_rect(point, 258.0f, 160.0f, 646.0f, 600.0f)) {
        return -1;
    }
    const int visible = (point.y - 160) / kToolbarRowHeight;
    const int index = state->toolbar_scroll_offset + visible;
    const auto tools = split_by_comma(state->config.toolbar_order);
    return index >= 0 && index < static_cast<int>(tools.size()) ? index : -1;
}

bool toolbar_handle_at_point(const SettingsState* state, POINT point) {
    return point.x >= 262 && point.x <= 292 &&
           toolbar_index_at_point(state, point) >= 0;
}

std::optional<SettingsFocusTarget> hit_test_settings(
    const SettingsState* state,
    POINT point) {
    auto available = [state](SettingsFocusTarget target)
        -> std::optional<SettingsFocusTarget> {
        return settings_focus_target_available(state, target)
                   ? std::optional<SettingsFocusTarget>(target)
                   : std::nullopt;
    };

    if (point_in_rect(point, 872.0f, 0.0f, 920.0f, 48.0f)) {
        return SettingsFocusTarget{SettingsFocusKind::close};
    }
    for (int index = 0; index < kCategoryCount; ++index) {
        const float top = static_cast<float>(kCategoryTop + index * kCategoryPitch);
        if (point_in_rect(point, 12.0f, top, 212.0f, top + 40.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::category, index};
        }
    }
    if (point_in_rect(point, 776.0f, 670.0f, 888.0f, 710.0f)) {
        if (settings_are_dirty(state) && !state->shortcut_error) {
            return SettingsFocusTarget{SettingsFocusKind::save};
        }
        return std::nullopt;
    }
    if (point_in_rect(point, 672.0f, 670.0f, 764.0f, 710.0f)) {
        return SettingsFocusTarget{SettingsFocusKind::cancel};
    }

    if (state->active_tab == 0) {
        if (point_in_rect(point, 258.0f, 226.0f, 886.0f, 282.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::capture_annotation};
        }
        if (point_in_rect(point, 258.0f, 282.0f, 886.0f, 338.0f)) {
            return available({SettingsFocusKind::capture_locked_tool});
        }
        if (point_in_rect(point, 594.0f, 429.0f, 724.0f, 465.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::capture_output, 0};
        }
        if (point_in_rect(point, 732.0f, 429.0f, 870.0f, 465.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::capture_output, 1};
        }
        if (point_in_rect(point, 258.0f, 478.0f, 886.0f, 534.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::capture_notifications};
        }
        if (state->config.annotation_next_serial != 1 &&
            point_in_rect(point, 766.0f, 544.0f, 870.0f, 580.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::reset_serial};
        }
    } else if (state->active_tab == 1) {
        if (point_in_rect(point, 258.0f, 150.0f, 886.0f, 206.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::ocr_enabled};
        }
        if (state->config.ocr_enabled && !state->is_downloading) {
            for (int index = 0;
                 index < static_cast<int>(kOcrEngineButtons.size());
                 ++index) {
                const auto& button =
                    kOcrEngineButtons[static_cast<std::size_t>(index)];
                if (point_in_rect(
                        point,
                        static_cast<float>(button.left),
                        222.0f,
                        static_cast<float>(button.right),
                        258.0f)) {
                    return SettingsFocusTarget{
                        SettingsFocusKind::ocr_engine, index};
                }
            }
            if (state->ocr_dependency.can_download &&
                point_in_rect(point, 746.0f, 294.0f, 870.0f, 330.0f)) {
                return SettingsFocusTarget{SettingsFocusKind::download_ocr};
            }
        }
        if (point_in_rect(point, 630.0f, 428.0f, 870.0f, 464.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::font_family};
        }
        if (point_in_rect(point, 736.0f, 484.0f, 796.0f, 520.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::text_bold};
        }
        if (point_in_rect(point, 808.0f, 484.0f, 870.0f, 520.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::text_italic};
        }
    } else if (state->active_tab == 2) {
        const int item = toolbar_index_at_point(state, point);
        if (item >= 0) {
            return SettingsFocusTarget{SettingsFocusKind::toolbar_item, item};
        }
        if (point_in_rect(point, 674.0f, 190.0f, 884.0f, 246.0f)) {
            return available({SettingsFocusKind::toolbar_visibility});
        }
        if (point_in_rect(point, 674.0f, 274.0f, 776.0f, 312.0f)) {
            return available({SettingsFocusKind::toolbar_move_up});
        }
        if (point_in_rect(point, 784.0f, 274.0f, 886.0f, 312.0f)) {
            return available({SettingsFocusKind::toolbar_move_down});
        }
    } else if (state->active_tab == 3) {
        if (point_in_rect(point, 258.0f, 192.0f, 886.0f, 244.0f)) {
            return available({SettingsFocusKind::global_ocr_enabled});
        }
        for (int index = 0; index < 3; ++index) {
            const float top = 244.0f + index * 52.0f;
            if (point_in_rect(point, 684.0f, top + 8.0f, 870.0f, top + 44.0f)) {
                return SettingsFocusTarget{SettingsFocusKind::shortcut, index};
            }
        }
        for (int index = 3; index < shortcut_count; ++index) {
            const int tool_index = index - 3;
            const int column = tool_index / 6;
            const int row = tool_index % 6;
            const float left = column == 0 ? 420.0f : 736.0f;
            const float top = 436.0f + row * 36.0f;
            if (point_in_rect(
                    point, left, top + 3.0f, left + 134.0f, top + 33.0f)) {
                return SettingsFocusTarget{SettingsFocusKind::shortcut, index};
            }
        }
    } else if (state->active_tab == 4) {
        if (point_in_rect(point, 258.0f, 152.0f, 886.0f, 208.0f)) {
            return SettingsFocusTarget{SettingsFocusKind::app_shell};
        }
        if (point_in_rect(point, 258.0f, 208.0f, 886.0f, 264.0f)) {
            return available({SettingsFocusKind::app_startup});
        }
        constexpr std::array<std::pair<int, int>, 3> bounds{{
            {486, 608},
            {616, 738},
            {746, 870},
        }};
        for (int index = 0; index < static_cast<int>(bounds.size()); ++index) {
            const auto [left, right] = bounds[static_cast<std::size_t>(index)];
            if (point_in_rect(
                    point,
                    static_cast<float>(left),
                    344.0f,
                    static_cast<float>(right),
                    382.0f)) {
                return SettingsFocusTarget{SettingsFocusKind::theme, index};
            }
        }
        constexpr std::array<std::pair<int, int>, 3> icon_bounds{{
            {276, 468},
            {476, 668},
            {676, 868},
        }};
        for (int index = 0; index < static_cast<int>(icon_bounds.size()); ++index) {
            const auto [left, right] = icon_bounds[static_cast<std::size_t>(index)];
            if (point_in_rect(
                    point,
                    static_cast<float>(left),
                    470.0f,
                    static_cast<float>(right),
                    552.0f)) {
                return SettingsFocusTarget{SettingsFocusKind::app_icon, index};
            }
        }
    }
    return std::nullopt;
}

void move_toolbar_item(SettingsState* state, int source, int destination) {
    auto tools = split_by_comma(state->config.toolbar_order);
    if (source < 0 || destination < 0 ||
        source >= static_cast<int>(tools.size()) ||
        destination >= static_cast<int>(tools.size()) ||
        source == destination) {
        return;
    }
    std::wstring item = std::move(tools[static_cast<std::size_t>(source)]);
    tools.erase(tools.begin() + source);
    tools.insert(tools.begin() + destination, std::move(item));
    state->config.toolbar_order = join_by_comma(tools);
    state->selected_tool_idx = destination;
    state->keyboard_focus =
        SettingsFocusTarget{SettingsFocusKind::toolbar_item, destination};
}

bool record_settings_shortcut(SettingsState* state, WPARAM key) {
    if (state->capturing_idx_ < 0 || state->capturing_idx_ >= shortcut_count) {
        return false;
    }
    if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU ||
        key == VK_LWIN || key == VK_RWIN) {
        return true;
    }
    if (key == VK_ESCAPE) {
        state->capturing_idx_ = -1;
        InvalidateRect(state->window, nullptr, FALSE);
        return true;
    }

    std::wstring shortcut;
    if (key != VK_DELETE && key != VK_BACK) {
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            shortcut += L"Ctrl+";
        }
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
            shortcut += L"Alt+";
        }
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
            shortcut += L"Shift+";
        }
        if ((GetKeyState(VK_LWIN) & 0x8000) != 0 ||
            (GetKeyState(VK_RWIN) & 0x8000) != 0) {
            shortcut += L"Win+";
        }

        if (key == VK_SNAPSHOT) {
            shortcut += L"PrintScreen";
        } else if ((key >= 'A' && key <= 'Z') ||
                   (key >= '0' && key <= '9')) {
            shortcut += static_cast<wchar_t>(key);
        } else if (key >= VK_F1 && key <= VK_F24) {
            shortcut += std::format(L"F{}", key - VK_F1 + 1);
        } else {
            return true;
        }
    }

    if (std::wstring* value =
            get_shortcut_ptr(state->config, state->capturing_idx_)) {
        *value = std::move(shortcut);
    }
    state->capturing_idx_ = -1;
    state->shortcut_error = shortcut_validation_error(state->config);
    InvalidateRect(state->window, nullptr, TRUE);
    return true;
}

void update_toolbar_drag(SettingsState* state, POINT point) {
    auto tools = split_by_comma(state->config.toolbar_order);
    const int max_offset =
        std::max(0, static_cast<int>(tools.size()) - kToolbarVisibleRows);
    if (point.y < 178 && state->toolbar_scroll_offset > 0) {
        --state->toolbar_scroll_offset;
    } else if (point.y > 590 && state->toolbar_scroll_offset < max_offset) {
        ++state->toolbar_scroll_offset;
    }
    const int visible = std::clamp(
        (static_cast<int>(point.y) - 160 + kToolbarRowHeight / 2) /
            kToolbarRowHeight,
        0,
        kToolbarVisibleRows - 1);
    state->drag_target_idx = std::clamp(
        state->toolbar_scroll_offset + visible,
        0,
        std::max(0, static_cast<int>(tools.size()) - 1));
}

LRESULT CALLBACK settings_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param) {
    auto* state =
        reinterpret_cast<SettingsState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<SettingsState*>(create->lpCreateParams);
        state->window = window;
        state->download_subscription = g_ocr_download.subscribe(window);
        refresh_ocr_download_state(state, true);
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
        case WM_CREATE:
            return 0;

        case kOcrDownloadStateChanged:
            if (static_cast<UINT_PTR>(w_param) ==
                state->download_subscription) {
                refresh_ocr_download_state(state);
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;

        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            refresh_settings_theme(state);
            return 0;

        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTTAB;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            if (ensure_resources(state)) {
                state->render_target->BeginDraw();
                paint_settings(state);
                const HRESULT result = state->render_target->EndDraw();
                if (result == D2DERR_RECREATE_TARGET) {
                    discard_resources(state);
                }
            }
            EndPaint(window, &paint);
            return 0;
        }

        case WM_SIZE:
            if (state->render_target) {
                discard_resources(state);
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;

        case WM_MOUSEMOVE: {
            const POINT point = settings_point_from_client(window, l_param);
            state->mouse_pos = point;
            TRACKMOUSEEVENT tracking{
                sizeof(tracking), TME_LEAVE, window, HOVER_DEFAULT};
            TrackMouseEvent(&tracking);
            if (state->dragging_tool_idx >= 0) {
                if ((w_param & MK_LBUTTON) != 0) {
                    update_toolbar_drag(state, point);
                } else {
                    state->dragging_tool_idx = -1;
                    state->drag_target_idx = -1;
                    if (GetCapture() == window) {
                        ReleaseCapture();
                    }
                }
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSELEAVE:
            state->mouse_pos = {-100, -100};
            if (state->dragging_tool_idx < 0) {
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            SetFocus(window);
            const POINT point = settings_point_from_client(window, l_param);
            state->mouse_pos = point;
            state->keyboard_focus_visible = false;

            if (point_in_rect(point, 16.0f, 574.0f, 208.0f, 638.0f)) {
                state->active_tab = 3;
                state->capturing_idx_ = idx_capture_hotkey;
                state->keyboard_focus = SettingsFocusTarget{
                    SettingsFocusKind::shortcut, idx_capture_hotkey};
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            }
            if (point.y >= 0 && point.y < kTitleBarHeight &&
                point.x < 872) {
                ReleaseCapture();
                SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }

            if (state->active_tab == 2) {
                const int item = toolbar_index_at_point(state, point);
                if (item >= 0 && point.x >= 262 && point.x <= 292) {
                    state->selected_tool_idx = item;
                    state->dragging_tool_idx = item;
                    state->drag_target_idx = item;
                    state->keyboard_focus = SettingsFocusTarget{
                        SettingsFocusKind::toolbar_item, item};
                    SetCapture(window);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (item >= 0 && point.x >= 294 && point.x <= 324) {
                    const auto tools =
                        split_by_comma(state->config.toolbar_order);
                    state->selected_tool_idx = item;
                    toggle_hidden_tool(
                        state->config.annotation_hidden_tools,
                        tools[static_cast<std::size_t>(item)]);
                    state->keyboard_focus = SettingsFocusTarget{
                        SettingsFocusKind::toolbar_item, item};
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
            }

            const auto target = hit_test_settings(state, point);
            if (target) {
                state->keyboard_focus = *target;
                (void)activate_settings_focus(state, *target);
                return 0;
            }
            if (state->capturing_idx_ >= 0) {
                state->capturing_idx_ = -1;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (state->dragging_tool_idx >= 0) {
                if (state->drag_target_idx >= 0) {
                    move_toolbar_item(
                        state,
                        state->dragging_tool_idx,
                        state->drag_target_idx);
                }
                state->dragging_tool_idx = -1;
                state->drag_target_idx = -1;
                if (GetCapture() == window) {
                    ReleaseCapture();
                }
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(l_param) != window) {
                state->dragging_tool_idx = -1;
                state->drag_target_idx = -1;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (state->active_tab == 2) {
                auto tools = split_by_comma(state->config.toolbar_order);
                const int max_offset =
                    std::max(0, static_cast<int>(tools.size()) - kToolbarVisibleRows);
                const int notches =
                    std::max(1, std::abs(GET_WHEEL_DELTA_WPARAM(w_param)) /
                                    WHEEL_DELTA);
                const int direction =
                    GET_WHEEL_DELTA_WPARAM(w_param) > 0 ? -1 : 1;
                state->toolbar_scroll_offset = std::clamp(
                    state->toolbar_scroll_offset + direction * notches,
                    0,
                    max_offset);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (record_settings_shortcut(state, w_param)) {
                return 0;
            }
            if (message == WM_KEYDOWN && w_param == VK_TAB) {
                move_settings_keyboard_focus(
                    state, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                return 0;
            }
            if (message == WM_KEYDOWN &&
                (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                w_param == 'S') {
                if (settings_are_dirty(state) && !state->shortcut_error) {
                    (void)accept_settings(state);
                }
                return 0;
            }
            if (state->active_tab == 2 &&
                (GetKeyState(VK_MENU) & 0x8000) != 0 &&
                (w_param == VK_UP || w_param == VK_DOWN) &&
                state->selected_tool_idx >= 0) {
                const SettingsFocusTarget target{
                    w_param == VK_UP
                        ? SettingsFocusKind::toolbar_move_up
                        : SettingsFocusKind::toolbar_move_down};
                if (settings_focus_target_available(state, target)) {
                    (void)activate_settings_focus(state, target);
                }
                return 0;
            }
            if (message == WM_KEYDOWN && state->keyboard_focus_visible &&
                state->keyboard_focus &&
                (w_param == VK_LEFT || w_param == VK_RIGHT ||
                 w_param == VK_UP || w_param == VK_DOWN)) {
                SettingsFocusTarget target = *state->keyboard_focus;
                const int delta =
                    (w_param == VK_LEFT || w_param == VK_UP) ? -1 : 1;
                int count = 0;
                if (target.kind == SettingsFocusKind::category) {
                    count = kCategoryCount;
                } else if (target.kind == SettingsFocusKind::capture_output) {
                    count = 2;
                } else if (target.kind == SettingsFocusKind::ocr_engine ||
                           target.kind == SettingsFocusKind::theme ||
                           target.kind == SettingsFocusKind::app_icon) {
                    count = 3;
                }
                if (count > 0) {
                    target.index = (target.index + delta + count) % count;
                    state->keyboard_focus = target;
                    (void)activate_settings_focus(state, target);
                    state->keyboard_focus_visible = true;
                    return 0;
                }
            }
            if (message == WM_KEYDOWN &&
                (w_param == VK_RETURN || w_param == VK_SPACE)) {
                if ((l_param & (1LL << 30)) != 0) {
                    return 0;
                }
                if (state->keyboard_focus_visible && state->keyboard_focus &&
                    settings_focus_target_available(
                        state, *state->keyboard_focus)) {
                    (void)activate_settings_focus(
                        state, *state->keyboard_focus);
                } else if (w_param == VK_RETURN &&
                           settings_are_dirty(state) &&
                           !state->shortcut_error) {
                    (void)accept_settings(state);
                }
                return 0;
            }
            if (message == WM_KEYDOWN && w_param == VK_ESCAPE) {
                PostMessageW(window, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        }

        case WM_SETCURSOR:
            if (LOWORD(l_param) == HTCLIENT) {
                const POINT point = settings_point_from_cursor(window);
                if (toolbar_handle_at_point(state, point)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                    return TRUE;
                }
                if (hit_test_settings(state, point) ||
                    point_in_rect(point, 16.0f, 574.0f, 208.0f, 638.0f)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(l_param);
            const HMONITOR monitor =
                MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST);
            const RECT work_area = settings_work_area(monitor);
            const SIZE size = fitted_settings_size(HIWORD(w_param), work_area);
            const LONG min_x = work_area.left + kWorkAreaMargin;
            const LONG min_y = work_area.top + kWorkAreaMargin;
            const LONG max_x =
                work_area.right - kWorkAreaMargin - size.cx;
            const LONG max_y =
                work_area.bottom - kWorkAreaMargin - size.cy;
            const int x = static_cast<int>(std::clamp(
                suggested->left, min_x, std::max(min_x, max_x)));
            const int y = static_cast<int>(std::clamp(
                suggested->top, min_y, std::max(min_y, max_y)));
            SetWindowPos(
                window,
                nullptr,
                x,
                y,
                size.cx,
                size.cy,
                SWP_NOZORDER | SWP_NOACTIVATE);
            discard_resources(state);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }

        case WM_CLOSE:
            discard_resources(state);
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            return 0;

        case WM_NCDESTROY: {
            if (state->download_subscription != 0) {
                g_ocr_download.unsubscribe(
                    window, state->download_subscription);
                state->download_subscription = 0;
            }
            state->window = nullptr;
            state->nc_destroyed = true;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            const LRESULT result =
                DefWindowProcW(window, message, w_param, l_param);
            restore_settings_owner(state);
            if (state->modal_depth == 0) {
                finalize_settings_state(state);
            }
            return result;
        }
    }
    return DefWindowProcW(window, message, w_param, l_param);
}


} // namespace

HWND show_settings_window_async(HWND owner, AppConfig config, SettingsWindowCompletion completion) {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.style = CS_DROPSHADOW;
        window_class.lpfnWndProc = settings_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        window_class.lpszClassName = L"AirScreenshot.Settings";
        RegisterClassExW(&window_class);
    });

    auto* state = new (std::nothrow) SettingsState;
    if (!state) {
        if (completion) {
            completion(std::nullopt);
        }
        return nullptr;
    }
    state->config = std::move(config);
    state->config.default_output =
        _wcsicmp(state->config.default_output.c_str(), L"file") == 0 ? L"file" : L"clipboard";
    state->initial_config = state->config;
    state->is_light_theme = should_use_light_theme(state->config.theme);
    state->high_contrast = system_high_contrast_enabled();
    state->shortcut_error = shortcut_validation_error(state->config);
    if (!split_by_comma(state->config.toolbar_order).empty()) {
        state->selected_tool_idx = 0;
    }
    state->owner = owner;
    state->completion = std::move(completion);
    UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (dpi == 0) {
        dpi = 96;
    }

    RECT owner_rect{};
    const bool has_visible_owner =
        owner && IsWindowVisible(owner) && GetWindowRect(owner, &owner_rect);
    const HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
    const RECT work_area = settings_work_area(monitor);
    const SIZE window_size = fitted_settings_size(dpi, work_area);
    auto clamp_position = [](LONG value, LONG size, LONG minimum, LONG maximum) {
        if (size >= maximum - minimum) {
            return static_cast<int>(minimum);
        }
        return static_cast<int>(std::clamp(value, minimum, maximum - size));
    };
    const LONG preferred_x =
        has_visible_owner
            ? owner_rect.left + ((owner_rect.right - owner_rect.left) - window_size.cx) / 2
            : work_area.left + ((work_area.right - work_area.left) - window_size.cx) / 2;
    const LONG preferred_y =
        has_visible_owner
            ? owner_rect.top + ((owner_rect.bottom - owner_rect.top) - window_size.cy) / 2
            : work_area.top + ((work_area.bottom - work_area.top) - window_size.cy) / 2;
    const int x = clamp_position(
        preferred_x,
        window_size.cx,
        work_area.left + kWorkAreaMargin,
        work_area.right - kWorkAreaMargin);
    const int y = clamp_position(
        preferred_y,
        window_size.cy,
        work_area.top + kWorkAreaMargin,
        work_area.bottom - kWorkAreaMargin);

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.Settings",
                                  strings::settings_title.data(),
                                  WS_POPUP | WS_SYSMENU,
                                  x,
                                  y,
                                  window_size.cx,
                                  window_size.cy,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  state);
    if (!window) {
        auto failed_completion = std::move(state->completion);
        delete state;
        if (failed_completion) {
            failed_completion(std::nullopt);
        }
        return nullptr;
    }

    apply_app_icon_to_window(window, state->config.app_icon);

    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(window, &margins);

    BOOL use_dark = !state->is_light_theme && !state->high_contrast;
    DwmSetWindowAttribute(window, 20, &use_dark, sizeof(use_dark));
    DwmSetWindowAttribute(window, 19, &use_dark, sizeof(use_dark));
    DWORD corner_preference = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(window, 33, &corner_preference, sizeof(corner_preference));
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return window;
}

bool show_settings_window(HWND owner, AppConfig& config) {
    bool completed = false;
    std::optional<AppConfig> result;
    HWND window = show_settings_window_async(
        owner,
        config,
        [&](std::optional<AppConfig> edited) {
            result = std::move(edited);
            completed = true;
        });
    if (!window && !completed) {
        return false;
    }

    MSG message{};
    while (!completed) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            if (window && IsWindow(window)) {
                DestroyWindow(window);
            }
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result) {
        config = std::move(*result);
        return true;
    }
    return false;
}

} // namespace airshot
