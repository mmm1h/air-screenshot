#include "settings_window.h"

#include "airshot/ocr.h"
#include "airshot/strings.h"
#include "airshot/ui_theme.h"
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

namespace airshot {
namespace {

constexpr int kSettingsWidth = 740;
constexpr int kSettingsHeight = 760;
constexpr int kWorkAreaMargin = 8;
constexpr int kGeneralSwitchCount = 8;
constexpr int kGeneralRowHeight = 30;

enum class SettingsFocusKind {
    tab,
    general_switch,
    font_family,
    text_bold,
    text_italic,
    ocr_engine,
    download_ocr,
    theme,
    toolbar_item,
    toolbar_visibility,
    toolbar_move_up,
    toolbar_move_down,
    shortcut,
    output_clipboard,
    output_file,
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
    idx_pin_hotkey,
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
    SettingsWindowValidator validator;
    bool is_light_theme{};
    bool high_contrast{};
    UiPalette palette{};
    std::wstring ui_font_family{L"Segoe UI"};
    
    // UI state
    int active_tab{0}; // 0: 常规设置, 1: 工具栏, 2: 快捷键
    int capturing_idx_{-1}; // capturing hotkey index
    int selected_tool_idx{-1};
    POINT mouse_pos{};
    std::optional<SettingsFocusTarget> keyboard_focus;
    bool keyboard_focus_visible{};

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

    // Text Formats
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> small_format;
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
        case idx_pin_hotkey: return &config.pin_hotkey;
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
        case idx_pin_hotkey: return &config.pin_hotkey;
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
    L"剪贴板贴图",
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
            if (index == idx_capture_hotkey) {
                return L"“截图”的快捷键不能为空。";
            }
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

bool toggle_tray_icon(SettingsState* state) {
    if (!state->config.shell_enabled) {
        return true;
    }
    if (state->config.tray_icon_visible) {
        enter_settings_modal(state);
        const int choice = MessageBoxW(
            state->window,
            L"隐藏托盘图标后，Air Screenshot 仍会在后台运行，截图快捷键也会继续生效。\n\n"
            L"如需恢复图标，请运行：\n"
            L"AirScreenshot.exe app settings\n\n"
            L"确定隐藏托盘图标吗？",
            L"隐藏托盘图标",
            MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2);
        if (!leave_settings_modal(state)) {
            return false;
        }
        if (choice != IDYES) {
            return true;
        }
    }
    state->config.tray_icon_visible = !state->config.tray_icon_visible;
    return true;
}

bool accept_settings(SettingsState* state) {
    if (const auto validation_error = shortcut_validation_error(state->config)) {
        state->active_tab = 2;
        enter_settings_modal(state);
        MessageBoxW(state->window,
                    validation_error->c_str(),
                    L"快捷键冲突",
                    MB_OK | MB_ICONWARNING);
        if (!leave_settings_modal(state)) {
            return false;
        }
        InvalidateRect(state->window, nullptr, TRUE);
        return true;
    }
    if (state->validator) {
        std::wstring validation_error;
        if (!state->validator(state->config, &validation_error)) {
            state->active_tab = 2;
            enter_settings_modal(state);
            MessageBoxW(
                state->window,
                validation_error.empty()
                    ? L"所选全局快捷键当前不可用，请换一个组合。"
                    : validation_error.c_str(),
                L"快捷键不可用",
                MB_OK | MB_ICONWARNING);
            if (!leave_settings_modal(state)) {
                return false;
            }
            InvalidateRect(state->window, nullptr, TRUE);
            return true;
        }
    }
    state->accepted = true;
    PostMessageW(state->window, WM_CLOSE, 0, 0);
    return true;
}

struct OcrEngineButton {
    std::wstring_view engine;
    const wchar_t* label;
    int left;
    int right;
};

constexpr std::array<OcrEngineButton, 3> kOcrEngineButtons{{
    {kOcrEngineRapidV5Fast, L"极速 OCR", 250, 380},
    {kOcrEngineRapidV5Accurate, L"高精度 OCR", 390, 520},
    {kOcrEngineRapidV4Compat, L"兼容 OCR", 530, 660},
}};

const OcrEngineButton* hit_test_ocr_engine_button(POINT pt) {
    if (pt.y < 525 || pt.y > 555) {
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
    if (id == L"lock") return L"锁定工具 (Lock Tool)";
    if (id == L"select") return L"选择工具 (Select Tool)";
    if (id == L"rect") return L"矩形工具 (Rectangle Tool)";
    if (id == L"ellipse") return L"椭圆工具 (Ellipse Tool)";
    if (id == L"line") return L"直线工具 (Line Tool)";
    if (id == L"arrow") return L"箭头工具 (Arrow Tool)";
    if (id == L"pen") return L"画笔工具 (Pen Tool)";
    if (id == L"mosaic") return L"马赛克工具 (Mosaic Tool)";
    if (id == L"blur") return L"模糊工具 (Blur Tool)";
    if (id == L"highlight") return L"高亮工具 (Highlight Tool)";
    if (id == L"watermark") return L"水印工具 (Watermark Tool)";
    if (id == L"text") return L"文本工具 (Text Tool)";
    if (id == L"serial") return L"步骤序号 (Step Serial)";
    if (id == L"eraser") return L"橡皮擦 (Eraser)";
    if (id == L"undo") return L"撤销按钮 (Undo Button)";
    if (id == L"redo") return L"重做按钮 (Redo Button)";
    if (id == L"ocr") return L"屏幕识字 (OCR)";
    if (id == L"scroll") return L"长图滚动 (Scroll Capture)";
    if (id == L"pin") return L"钉图 (Pin Sticker)";
    if (id == L"copy") return L"复制 (Copy)";
    if (id == L"save") return L"保存 (Save)";
    if (id == L"close") return L"关闭 (Close)";
    return std::wstring(id);
}

void discard_resources(SettingsState* state);

void refresh_settings_theme(SettingsState* state) {
    state->palette = resolve_ui_palette(state->config.theme);
    state->is_light_theme = state->palette.light;
    state->high_contrast = state->palette.high_contrast;
    discard_resources(state);
    if (state->window) {
        BOOL use_dark = !state->is_light_theme && !state->high_contrast;
        DwmSetWindowAttribute(state->window, 20, &use_dark, sizeof(use_dark));
        DwmSetWindowAttribute(state->window, 19, &use_dark, sizeof(use_dark));
        InvalidateRect(state->window, nullptr, TRUE);
    }
}

bool ensure_resources(SettingsState* state) {
    if (state->render_target) {
        return true;
    }

    if (state->window) {
        BOOL use_dark = !state->is_light_theme && !state->high_contrast;
        DwmSetWindowAttribute(state->window, 20, &use_dark, sizeof(use_dark));
        DwmSetWindowAttribute(state->window, 19, &use_dark, sizeof(use_dark));
    }

    RECT rect{};
    if (!GetClientRect(state->window, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) {
        return false;
    }
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(rect.right - rect.left),
                                         static_cast<UINT32>(rect.bottom - rect.top));
    const auto fail = [state] {
        discard_resources(state);
        return false;
    };

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(state->d2d_factory.GetAddressOf()));
    if (FAILED(hr)) return fail();

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(state->dwrite_factory.GetAddressOf()));
    if (FAILED(hr)) return fail();

    hr = state->d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(state->window, size),
        state->render_target.GetAddressOf()
    );
    if (FAILED(hr)) return fail();

    const float dpi = 96.0f * settings_layout_scale(state->window);
    state->render_target->SetDpi(dpi, dpi);

    // Create brushes from the shared system-aware palette.
    const UiPalette& palette = state->palette;
    state->render_target->CreateSolidColorBrush(palette.background, state->bg_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.sidebar, state->sidebar_bg_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.text, state->text_white_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.muted, state->text_grey_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.accent, state->blue_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.accent_hover, state->hover_blue_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.accent_text, state->accent_text_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.border, state->border_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.active, state->active_tab_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.control, state->control_bg_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.switch_off, state->switch_track_off_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.card, state->card_bg_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.card_border, state->card_border_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.separator, state->separator_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.hover, state->hover_bg_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.cancel_border, state->cancel_btn_border_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.accent_indicator, state->accent_indicator_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.keycap_shadow, state->keycap_shadow_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.switch_glow, state->switch_glow_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.danger_surface, state->close_btn_hover_bg_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(palette.danger, state->red_brush.GetAddressOf());
    if (!state->bg_brush || !state->sidebar_bg_brush || !state->text_white_brush ||
        !state->text_grey_brush || !state->blue_brush || !state->hover_blue_brush ||
        !state->accent_text_brush ||
        !state->border_brush || !state->active_tab_brush || !state->control_bg_brush ||
        !state->switch_track_off_brush || !state->card_bg_brush || !state->card_border_brush ||
        !state->separator_brush || !state->hover_bg_brush || !state->cancel_btn_border_brush ||
        !state->accent_indicator_brush || !state->keycap_shadow_brush ||
        !state->switch_glow_brush || !state->close_btn_hover_bg_brush || !state->red_brush) {
        return fail();
    }

    // Use the Windows UI family and let font linking provide Chinese glyphs.
    state->ui_font_family =
        preferred_ui_font_family(state->dwrite_factory.Get());
    state->dwrite_factory->CreateTextFormat(state->ui_font_family.c_str(), nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-CN", state->title_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(state->ui_font_family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", state->text_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(state->ui_font_family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-CN", state->small_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", state->hotkey_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(state->ui_font_family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", state->btn_text_format.GetAddressOf());
    if (!state->title_format || !state->text_format || !state->small_format ||
        !state->hotkey_format || !state->btn_text_format) {
        return fail();
    }

    state->title_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->small_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->hotkey_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->hotkey_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    state->btn_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    state->title_format.Reset();
    state->text_format.Reset();
    state->small_format.Reset();
    state->hotkey_format.Reset();
    state->btn_text_format.Reset();
}

enum ButtonStyle {
    btn_primary,
    btn_secondary
};

void draw_switch(SettingsState* state, int x, int y, bool is_on) {
    // Glow effect
    if (is_on) {
        D2D1_RECT_F glow_rect = D2D1::RectF(
            static_cast<float>(x - 2),
            static_cast<float>(y - 2),
            static_cast<float>(x + 46),
            static_cast<float>(y + 24)
        );
        state->render_target->FillRoundedRectangle(D2D1::RoundedRect(glow_rect, 13.0f, 13.0f), state->switch_glow_brush.Get());
    }

    D2D1_RECT_F track_rect = D2D1::RectF(
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(x + 44),
        static_cast<float>(y + 22)
    );
    D2D1_ROUNDED_RECT rounded_track = D2D1::RoundedRect(track_rect, 11.0f, 11.0f);
    
    if (is_on) {
        state->render_target->FillRoundedRectangle(rounded_track, state->blue_brush.Get());
    } else {
        state->render_target->FillRoundedRectangle(rounded_track, state->switch_track_off_brush.Get());
    }

    float thumb_x = is_on ? (x + 31.0f) : (x + 11.0f);
    D2D1_ELLIPSE thumb = D2D1::Ellipse(
        D2D1::Point2F(thumb_x, y + 11.0f),
        8.0f,
        8.0f
    );
    state->render_target->FillEllipse(thumb, state->accent_text_brush.Get());
}

void draw_button(SettingsState* state, int x1, int y1, int x2, int y2, const wchar_t* label, bool is_hovered, ButtonStyle style = btn_primary) {
    D2D1_RECT_F rect = D2D1::RectF(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(x2), static_cast<float>(y2));
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, 6.0f, 6.0f);
    
    if (style == btn_primary) {
        if (is_hovered) {
            state->render_target->FillRoundedRectangle(rounded, state->hover_blue_brush.Get());
            state->render_target->DrawRoundedRectangle(rounded, state->hover_blue_brush.Get(), 1.0f);
        } else {
            state->render_target->FillRoundedRectangle(rounded, state->blue_brush.Get());
            state->render_target->DrawRoundedRectangle(rounded, state->blue_brush.Get(), 1.0f);
        }
        state->render_target->DrawTextW(label, static_cast<UINT32>(wcslen(label)), state->btn_text_format.Get(), rect, state->accent_text_brush.Get());
    } else {
        if (is_hovered) {
            state->render_target->FillRoundedRectangle(rounded, state->hover_bg_brush.Get());
            state->render_target->DrawRoundedRectangle(rounded, state->hover_blue_brush.Get(), 1.0f);
        } else {
            state->render_target->FillRoundedRectangle(rounded, state->control_bg_brush.Get());
            state->render_target->DrawRoundedRectangle(rounded, state->cancel_btn_border_brush.Get(), 1.0f);
        }
        state->render_target->DrawTextW(label, static_cast<UINT32>(wcslen(label)), state->btn_text_format.Get(), rect, state->text_white_brush.Get());
    }
}

void draw_choice_button(SettingsState* state, int x1, int y1, int x2, int y2, const wchar_t* label, bool is_selected, bool is_hovered) {
    D2D1_RECT_F rect = D2D1::RectF(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(x2), static_cast<float>(y2));
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, 6.0f, 6.0f);

    if (is_selected) {
        state->render_target->FillRoundedRectangle(rounded, state->blue_brush.Get());
        state->render_target->DrawTextW(label, static_cast<UINT32>(wcslen(label)), state->btn_text_format.Get(), rect, state->accent_text_brush.Get());
    } else {
        state->render_target->FillRoundedRectangle(rounded, is_hovered ? state->hover_bg_brush.Get() : state->control_bg_brush.Get());
        state->render_target->DrawRoundedRectangle(rounded, state->card_border_brush.Get(), 1.0f);
        state->render_target->DrawTextW(label, static_cast<UINT32>(wcslen(label)), state->btn_text_format.Get(), rect, state->text_grey_brush.Get());
    }
}

void draw_hotkey_box(SettingsState* state, int x1, int y1, int x2, int y2, const wchar_t* hotkey_str, bool is_capturing, bool is_hovered) {
    D2D1_RECT_F shadow_rect = D2D1::RectF(static_cast<float>(x1), static_cast<float>(y1 + 2), static_cast<float>(x2), static_cast<float>(y2 + 2));
    state->render_target->FillRoundedRectangle(D2D1::RoundedRect(shadow_rect, 6.0f, 6.0f), state->keycap_shadow_brush.Get());

    D2D1_RECT_F rect = D2D1::RectF(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(x2), static_cast<float>(y2));
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, 6.0f, 6.0f);

    state->render_target->FillRoundedRectangle(rounded, state->control_bg_brush.Get());
    
    if (is_capturing) {
        state->render_target->DrawRoundedRectangle(rounded, state->blue_brush.Get(), 1.5f);
        state->render_target->DrawTextW(L"按下按键...", 7, state->hotkey_format.Get(), rect, state->blue_brush.Get());
    } else {
        if (is_hovered) {
            state->render_target->DrawRoundedRectangle(rounded, state->hover_blue_brush.Get(), 1.0f);
        } else {
            state->render_target->DrawRoundedRectangle(rounded, state->card_border_brush.Get(), 1.0f);
        }
        const bool configured = hotkey_str && hotkey_str[0] != L'\0';
        const wchar_t* display = configured ? hotkey_str : L"未设置";
        state->render_target->DrawTextW(
            display,
            static_cast<UINT32>(wcslen(display)),
            state->hotkey_format.Get(),
            rect,
            configured
                ? state->text_white_brush.Get()
                : state->text_grey_brush.Get());
    }
}

std::vector<SettingsFocusTarget> settings_focus_targets(const SettingsState* state) {
    std::vector<SettingsFocusTarget> targets;
    targets.reserve(32);

    for (int index = 0; index < 3; ++index) {
        targets.push_back({SettingsFocusKind::tab, index});
    }

    if (state->active_tab == 0) {
        for (int index = 0; index < kGeneralSwitchCount; ++index) {
            const bool available =
                (index != 2 || state->config.ocr_enabled) &&
                ((index != 4 && index != 5) || state->config.shell_enabled);
            if (available) {
                targets.push_back({SettingsFocusKind::general_switch, index});
            }
        }
        targets.push_back({SettingsFocusKind::font_family});
        targets.push_back({SettingsFocusKind::text_bold});
        targets.push_back({SettingsFocusKind::text_italic});
        if (!state->is_downloading) {
            for (int index = 0; index < static_cast<int>(kOcrEngineButtons.size()); ++index) {
                targets.push_back({SettingsFocusKind::ocr_engine, index});
            }
            if (state->ocr_dependency.can_download) {
                targets.push_back({SettingsFocusKind::download_ocr});
            }
        }
        for (int index = 0; index < 3; ++index) {
            targets.push_back({SettingsFocusKind::theme, index});
        }
    } else if (state->active_tab == 1) {
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
    } else if (state->active_tab == 2) {
        for (int index = 0; index < shortcut_count; ++index) {
            targets.push_back({SettingsFocusKind::shortcut, index});
        }
    }

    targets.push_back({SettingsFocusKind::output_clipboard});
    targets.push_back({SettingsFocusKind::output_file});
    targets.push_back({SettingsFocusKind::save});
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
    const SettingsFocusTarget& target) {
    D2D1_RECT_F rect{};
    float radius = 6.0f;
    switch (target.kind) {
        case SettingsFocusKind::tab: {
            const float top = 60.0f + target.index * 50.0f;
            rect = D2D1::RectF(10.0f, top, 190.0f, top + 38.0f);
            break;
        }
        case SettingsFocusKind::general_switch: {
            const float top = 110.0f + target.index * static_cast<float>(kGeneralRowHeight);
            rect = D2D1::RectF(232.0f, top + 2.0f, 708.0f, top + kGeneralRowHeight - 2.0f);
            break;
        }
        case SettingsFocusKind::font_family:
            rect = D2D1::RectF(250.0f, 405.0f, 690.0f, 435.0f);
            break;
        case SettingsFocusKind::text_bold:
            rect = D2D1::RectF(347.0f, 449.0f, 397.0f, 477.0f);
            radius = 14.0f;
            break;
        case SettingsFocusKind::text_italic:
            rect = D2D1::RectF(587.0f, 449.0f, 637.0f, 477.0f);
            radius = 14.0f;
            break;
        case SettingsFocusKind::ocr_engine:
            if (target.index < 0 ||
                target.index >= static_cast<int>(kOcrEngineButtons.size())) {
                return std::nullopt;
            }
            rect = D2D1::RectF(
                static_cast<float>(kOcrEngineButtons[static_cast<std::size_t>(target.index)].left),
                525.0f,
                static_cast<float>(kOcrEngineButtons[static_cast<std::size_t>(target.index)].right),
                555.0f);
            break;
        case SettingsFocusKind::download_ocr:
            rect = D2D1::RectF(530.0f, 565.0f, 650.0f, 595.0f);
            break;
        case SettingsFocusKind::theme: {
            constexpr std::array<std::pair<int, int>, 3> bounds{{
                {250, 380},
                {390, 520},
                {530, 660},
            }};
            if (target.index < 0 || target.index >= static_cast<int>(bounds.size())) {
                return std::nullopt;
            }
            const auto [left, right] = bounds[static_cast<std::size_t>(target.index)];
            rect = D2D1::RectF(
                static_cast<float>(left), 635.0f, static_cast<float>(right), 665.0f);
            break;
        }
        case SettingsFocusKind::toolbar_item: {
            const float top = 85.0f + target.index * 27.0f;
            rect = D2D1::RectF(232.0f, top, 478.0f, top + 26.0f);
            radius = 4.0f;
            break;
        }
        case SettingsFocusKind::toolbar_visibility:
            rect = D2D1::RectF(517.0f, 157.0f, 687.0f, 193.0f);
            break;
        case SettingsFocusKind::toolbar_move_up:
            rect = D2D1::RectF(520.0f, 220.0f, 690.0f, 255.0f);
            break;
        case SettingsFocusKind::toolbar_move_down:
            rect = D2D1::RectF(520.0f, 275.0f, 690.0f, 310.0f);
            break;
        case SettingsFocusKind::shortcut: {
            if (target.index < 0 || target.index >= shortcut_count) {
                return std::nullopt;
            }
            const bool first_column = target.index < 8;
            const int row = first_column ? target.index : target.index - 8;
            const float top = 95.0f + row * 65.0f;
            rect = D2D1::RectF(
                first_column ? 355.0f : 595.0f,
                top,
                first_column ? 455.0f : 695.0f,
                top + 26.0f);
            break;
        }
        case SettingsFocusKind::output_clipboard:
            rect = D2D1::RectF(230.0f, 715.0f, 350.0f, 745.0f);
            break;
        case SettingsFocusKind::output_file:
            rect = D2D1::RectF(360.0f, 715.0f, 490.0f, 745.0f);
            break;
        case SettingsFocusKind::save:
            rect = D2D1::RectF(500.0f, 715.0f, 600.0f, 745.0f);
            break;
        case SettingsFocusKind::cancel:
            rect = D2D1::RectF(615.0f, 715.0f, 715.0f, 745.0f);
            break;
        case SettingsFocusKind::close:
            rect = D2D1::RectF(697.0f, 2.0f, 738.0f, 48.0f);
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
    auto bounds = settings_focus_bounds(*state->keyboard_focus);
    if (!bounds) {
        return;
    }
    bounds->rect.left = std::max(2.0f, bounds->rect.left - 2.0f);
    bounds->rect.top = std::max(2.0f, bounds->rect.top - 2.0f);
    bounds->rect.right = std::min(738.0f, bounds->rect.right + 2.0f);
    bounds->rect.bottom = std::min(758.0f, bounds->rect.bottom + 2.0f);
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
        settings_scale_dip(state->window, 250),
        settings_scale_dip(state->window, 435),
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
        case SettingsFocusKind::tab:
            if (target.index >= 0 && target.index < 3) {
                state->active_tab = target.index;
                state->capturing_idx_ = -1;
            }
            break;
        case SettingsFocusKind::general_switch:
            switch (target.index) {
                case 0:
                    state->config.annotation_enabled = !state->config.annotation_enabled;
                    break;
                case 1:
                    state->config.ocr_enabled = !state->config.ocr_enabled;
                    if (!state->config.ocr_enabled) {
                        state->config.global_ocr_enabled = false;
                    }
                    break;
                case 2:
                    if (state->config.ocr_enabled) {
                        state->config.global_ocr_enabled =
                            !state->config.global_ocr_enabled;
                    }
                    break;
                case 3:
                    state->config.shell_enabled = !state->config.shell_enabled;
                    break;
                case 4:
                    if (!toggle_tray_icon(state)) {
                        return false;
                    }
                    break;
                case 5:
                    state->config.start_at_login = !state->config.start_at_login;
                    break;
                case 6:
                    state->config.notifications_enabled =
                        !state->config.notifications_enabled;
                    break;
                case 7:
                    state->config.annotation_locked_tool =
                        !state->config.annotation_locked_tool;
                    break;
                default:
                    break;
            }
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
        case SettingsFocusKind::shortcut:
            if (target.index >= 0 && target.index < shortcut_count) {
                state->capturing_idx_ = target.index;
            }
            break;
        case SettingsFocusKind::output_clipboard:
            state->config.default_output = L"clipboard";
            break;
        case SettingsFocusKind::output_file:
            state->config.default_output = L"file";
            break;
        case SettingsFocusKind::save:
            return accept_settings(state);
        case SettingsFocusKind::cancel:
        case SettingsFocusKind::close:
            PostMessageW(state->window, WM_CLOSE, 0, 0);
            return true;
    }
    InvalidateRect(state->window, nullptr, TRUE);
    return true;
}

LRESULT CALLBACK settings_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<SettingsState*>(create->lpCreateParams);
        state->window = window;
        state->download_subscription = g_ocr_download.subscribe(window);
        refresh_ocr_download_state(state, true);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
        case WM_CREATE: {
            return 0;
        }

        case kOcrDownloadStateChanged: {
            if (static_cast<UINT_PTR>(w_param) == state->download_subscription) {
                refresh_ocr_download_state(state);
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;
        }

        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED: {
            refresh_settings_theme(state);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(window, &ps);
            
            if (ensure_resources(state)) {
                state->render_target->BeginDraw();
                
                // Clear background
                state->render_target->Clear(state->palette.background);

                // Draw custom title bar (y: 0 to 50)
                D2D1_RECT_F title_bar_rect = D2D1::RectF(0.0f, 0.0f, 740.0f, 50.0f);
                state->render_target->FillRectangle(title_bar_rect, state->bg_brush.Get());
                
                // Title bar bottom border
                state->render_target->DrawLine(D2D1::Point2F(0.0f, 50.0f), D2D1::Point2F(740.0f, 50.0f), state->card_border_brush.Get(), 1.0f);

                // Draw title text
                D2D1_RECT_F title_text_rect = D2D1::RectF(16.0f, 0.0f, 400.0f, 50.0f);
                state->render_target->DrawTextW(strings::settings_title.data(), static_cast<UINT32>(strings::settings_title.size()), state->title_format.Get(), title_text_rect, state->text_white_brush.Get());

                // Draw custom close button (x: 695 to 740, y: 0 to 50)
                bool close_hovered = (state->mouse_pos.x >= 695 && state->mouse_pos.x <= 740 && state->mouse_pos.y >= 0 && state->mouse_pos.y <= 50);
                D2D1_RECT_F close_rect = D2D1::RectF(695.0f, 0.0f, 740.0f, 50.0f);
                if (close_hovered) {
                    state->render_target->FillRectangle(close_rect, state->close_btn_hover_bg_brush.Get());
                    float cx = 717.5f;
                    float cy = 25.0f;
                    state->render_target->DrawLine(D2D1::Point2F(cx - 5.0f, cy - 5.0f), D2D1::Point2F(cx + 5.0f, cy + 5.0f), state->red_brush.Get(), 1.5f);
                    state->render_target->DrawLine(D2D1::Point2F(cx + 5.0f, cy - 5.0f), D2D1::Point2F(cx - 5.0f, cy + 5.0f), state->red_brush.Get(), 1.5f);
                } else {
                    float cx = 717.5f;
                    float cy = 25.0f;
                    state->render_target->DrawLine(D2D1::Point2F(cx - 5.0f, cy - 5.0f), D2D1::Point2F(cx + 5.0f, cy + 5.0f), state->text_grey_brush.Get(), 1.5f);
                    state->render_target->DrawLine(D2D1::Point2F(cx + 5.0f, cy - 5.0f), D2D1::Point2F(cx - 5.0f, cy + 5.0f), state->text_grey_brush.Get(), 1.5f);
                }

                // 1. Draw Left Sidebar background (unified background)
                D2D1_RECT_F sidebar_rect = D2D1::RectF(0.0f, 50.0f, 200.0f, 700.0f);
                state->render_target->FillRectangle(sidebar_rect, state->bg_brush.Get());
                state->render_target->DrawLine(D2D1::Point2F(200.0f, 50.0f), D2D1::Point2F(200.0f, 700.0f), state->card_border_brush.Get(), 1.0f);

                // Sidebar Tab items
                const wchar_t* tab_labels[] = { L"常规设置", L"工具栏设置", L"全局与工具快捷键" };
                for (int i = 0; i < 3; ++i) {
                    float ty = 60.0f + i * 50.0f;
                    D2D1_RECT_F tab_rect = D2D1::RectF(10.0f, ty, 190.0f, ty + 38.0f);
                    bool is_hovered = (state->mouse_pos.x >= 10 && state->mouse_pos.x <= 190 && state->mouse_pos.y >= ty && state->mouse_pos.y <= ty + 38.0f);

                    if (state->active_tab == i) {
                        state->render_target->FillRoundedRectangle(D2D1::RoundedRect(tab_rect, 6.0f, 6.0f), state->hover_bg_brush.Get());
                        
                        // Left accent indicator pill
                        D2D1_RECT_F indicator = D2D1::RectF(14.0f, ty + 8.0f, 17.0f, ty + 30.0f);
                        state->render_target->FillRoundedRectangle(D2D1::RoundedRect(indicator, 1.5f, 1.5f), state->accent_indicator_brush.Get());

                        D2D1_RECT_F text_rect = D2D1::RectF(26.0f, ty, 190.0f, ty + 38.0f);
                        state->render_target->DrawTextW(tab_labels[i], static_cast<UINT32>(wcslen(tab_labels[i])), state->text_format.Get(), text_rect, state->blue_brush.Get());
                    } else {
                        if (is_hovered) {
                            state->render_target->FillRoundedRectangle(D2D1::RoundedRect(tab_rect, 6.0f, 6.0f), state->hover_bg_brush.Get());
                        }
                        D2D1_RECT_F text_rect = D2D1::RectF(26.0f, ty, 190.0f, ty + 38.0f);
                        state->render_target->DrawTextW(tab_labels[i], static_cast<UINT32>(wcslen(tab_labels[i])), state->text_format.Get(), text_rect, state->text_grey_brush.Get());
                    }
                }

                // 2. Draw Right Panel header based on active tab
                D2D1_RECT_F header_rect = D2D1::RectF(230.0f, 50.0f, 700.0f, 80.0f);
                const wchar_t* tab_titles[] = { L"常规功能配置", L"工具显示隐藏设置", L"快捷键配置与按键绑定" };
                state->render_target->DrawTextW(tab_titles[state->active_tab], static_cast<UINT32>(wcslen(tab_titles[state->active_tab])), state->title_format.Get(), header_rect, state->text_white_brush.Get());

                // 3. Draw Content Panel
                if (state->active_tab == 0) {
                    // Card 1: 功能开关
                    D2D1_RECT_F card1_title_rect = D2D1::RectF(230.0f, 85.0f, 700.0f, 105.0f);
                    state->render_target->DrawTextW(L"功能开关", 4, state->small_format.Get(), card1_title_rect, state->text_grey_brush.Get());

                    D2D1_RECT_F card1_rect = D2D1::RectF(230.0f, 110.0f, 710.0f, 350.0f);
                    D2D1_ROUNDED_RECT rounded_card1 = D2D1::RoundedRect(card1_rect, 8.0f, 8.0f);
                    state->render_target->FillRoundedRectangle(rounded_card1, state->card_bg_brush.Get());
                    state->render_target->DrawRoundedRectangle(rounded_card1, state->card_border_brush.Get(), 1.0f);

                    const wchar_t* labels[] = {
                        L"启用屏幕标注 (Screen Annotation)",
                        L"启用 OCR 识别 (OCR Recognition)",
                        L"启用全局 OCR 热键 (Global OCR Hotkey)",
                        L"运行后台服务与快捷键 (Background Service)",
                        L"显示系统托盘图标 (Show Tray Icon)",
                        L"开机自动启动 (Start at Login)",
                        L"启用提示通知 (Show Notifications)",
                        L"标注后保持当前工具 (Keep tool active)"
                    };
                    bool values[] = {
                        state->config.annotation_enabled,
                        state->config.ocr_enabled,
                        state->config.ocr_enabled && state->config.global_ocr_enabled,
                        state->config.shell_enabled,
                        state->config.tray_icon_visible,
                        state->config.start_at_login,
                        state->config.notifications_enabled,
                        state->config.annotation_locked_tool
                    };

                    for (int i = 0; i < kGeneralSwitchCount; ++i) {
                        float row_y = 110.0f + i * static_cast<float>(kGeneralRowHeight);
                        const bool row_enabled =
                            (i != 2 || state->config.ocr_enabled) &&
                            ((i != 4 && i != 5) || state->config.shell_enabled);
                        
                        // Row hover background
                        D2D1_RECT_F row_rect = D2D1::RectF(
                            231.0f, row_y + 1.0f, 709.0f, row_y + kGeneralRowHeight - 1.0f);
                        bool is_row_hovered =
                            state->mouse_pos.x >= 230 && state->mouse_pos.x <= 710 &&
                            state->mouse_pos.y >= row_y &&
                            state->mouse_pos.y <= row_y + kGeneralRowHeight;
                        if (is_row_hovered && row_enabled) {
                            D2D1_ROUNDED_RECT rounded_row = D2D1::RoundedRect(
                                row_rect, 
                                (i == 0 || i == kGeneralSwitchCount - 1) ? 7.0f : 0.0f,
                                (i == 0 || i == kGeneralSwitchCount - 1) ? 7.0f : 0.0f
                            );
                            state->render_target->FillRoundedRectangle(rounded_row, state->hover_bg_brush.Get());
                        }

                        // Label
                        D2D1_RECT_F text_rect = D2D1::RectF(
                            250.0f, row_y, 620.0f, row_y + kGeneralRowHeight);
                        state->render_target->DrawTextW(
                            labels[i],
                            static_cast<UINT32>(wcslen(labels[i])),
                            state->text_format.Get(),
                            text_rect,
                            row_enabled ? state->text_white_brush.Get()
                                        : state->text_grey_brush.Get());
                        
                        // Switch
                        draw_switch(state, 640, static_cast<int>(row_y) + 6, values[i]);

                        // Separator line
                        if (i < kGeneralSwitchCount - 1) {
                            state->render_target->DrawLine(
                                D2D1::Point2F(250.0f, row_y + kGeneralRowHeight),
                                D2D1::Point2F(690.0f, row_y + kGeneralRowHeight),
                                state->separator_brush.Get(), 
                                1.0f
                            );
                        }
                    }

                    // Card 2: 文本与 OCR
                    D2D1_RECT_F card2_title_rect = D2D1::RectF(230.0f, 365.0f, 700.0f, 385.0f);
                    state->render_target->DrawTextW(L"文本与 OCR 设置", 9, state->small_format.Get(), card2_title_rect, state->text_grey_brush.Get());

                    D2D1_RECT_F card2_rect = D2D1::RectF(230.0f, 390.0f, 710.0f, 680.0f);
                    D2D1_ROUNDED_RECT rounded_card2 = D2D1::RoundedRect(card2_rect, 8.0f, 8.0f);
                    state->render_target->FillRoundedRectangle(rounded_card2, state->card_bg_brush.Get());
                    state->render_target->DrawRoundedRectangle(rounded_card2, state->card_border_brush.Get(), 1.0f);

                    // Row 0: Font family
                    std::wstring font_desc = L"文本字体: " + state->config.text_font_family;
                    bool font_hovered = (state->mouse_pos.x >= 250 && state->mouse_pos.x <= 690 && state->mouse_pos.y >= 405 && state->mouse_pos.y <= 435);
                    draw_button(state, 250, 405, 690, 435, font_desc.c_str(), font_hovered, btn_secondary);

                    // Row 1: Text Bold & Italic
                    D2D1_RECT_F bold_label_rect = D2D1::RectF(250.0f, 445.0f, 340.0f, 480.0f);
                    state->render_target->DrawTextW(L"文本加粗 (Bold)", static_cast<UINT32>(wcslen(L"文本加粗 (Bold)")), state->small_format.Get(), bold_label_rect, state->text_white_brush.Get());
                    draw_switch(state, 350, 452, state->config.text_font_bold);

                    D2D1_RECT_F italic_label_rect = D2D1::RectF(490.0f, 445.0f, 580.0f, 480.0f);
                    state->render_target->DrawTextW(L"文本倾斜 (Italic)", static_cast<UINT32>(wcslen(L"文本倾斜 (Italic)")), state->small_format.Get(), italic_label_rect, state->text_white_brush.Get());
                    draw_switch(state, 590, 452, state->config.text_font_italic);

                    // Card 2 inner Separator 1
                    state->render_target->DrawLine(D2D1::Point2F(250.0f, 490.0f), D2D1::Point2F(690.0f, 490.0f), state->separator_brush.Get(), 1.0f);

                    // Row 2: OCR defaults & status
                    D2D1_RECT_F ocr_label_rect = D2D1::RectF(250.0f, 497.0f, 420.0f, 517.0f);
                    state->render_target->DrawTextW(L"OCR 默认引擎", static_cast<UINT32>(wcslen(L"OCR 默认引擎")), state->small_format.Get(), ocr_label_rect, state->text_grey_brush.Get());

                    const std::wstring current_ocr_engine = normalize_ocr_engine(state->config.ocr_engine);
                    for (const auto& button : kOcrEngineButtons) {
                        const bool selected = current_ocr_engine == button.engine;
                        const bool hovered = state->mouse_pos.x >= button.left && state->mouse_pos.x <= button.right &&
                                             state->mouse_pos.y >= 525 && state->mouse_pos.y <= 555;
                        draw_choice_button(state, button.left, 525, button.right, 555, button.label, selected, hovered);
                    }

                    const bool is_downloading = state->is_downloading;
                    const int progress = state->download_progress;
                    const std::wstring& dl_error = state->download_error;

                    std::wstring status_text = state->ocr_dependency.message;
                    bool needs_download = state->ocr_dependency.can_download;

                    if (is_downloading) {
                        status_text = std::format(L"正在下载... {}%", progress);
                    } else if (!dl_error.empty()) {
                        status_text = dl_error;
                    }

                    D2D1_RECT_F status_rect = D2D1::RectF(250.0f, 565.0f, 510.0f, 595.0f);
                    if (is_downloading) {
                        D2D1_RECT_F progress_track = D2D1::RectF(250.0f, 572.0f, 500.0f, 588.0f);
                        state->render_target->FillRectangle(progress_track, state->control_bg_brush.Get());
                        state->render_target->DrawRectangle(progress_track, state->card_border_brush.Get(), 1.0f);

                        float fill_width = 250.0f * (static_cast<float>(std::clamp(progress, 0, 100)) / 100.0f);
                        D2D1_RECT_F progress_fill = D2D1::RectF(250.0f, 572.0f, 250.0f + fill_width, 588.0f);
                        state->render_target->FillRectangle(progress_fill, state->blue_brush.Get());

                        D2D1_RECT_F text_rect = D2D1::RectF(515.0f, 565.0f, 700.0f, 595.0f);
                        state->render_target->DrawTextW(status_text.c_str(), static_cast<UINT32>(status_text.size()), state->small_format.Get(), text_rect, state->text_white_brush.Get());
                    } else if (needs_download) {
                        bool dl_hovered = (state->mouse_pos.x >= 530 && state->mouse_pos.x <= 650 && state->mouse_pos.y >= 565 && state->mouse_pos.y <= 595);
                        draw_button(state, 530, 565, 650, 595, L"下载依赖", dl_hovered, btn_primary);

                        state->render_target->DrawTextW(status_text.c_str(), static_cast<UINT32>(status_text.size()), state->small_format.Get(), status_rect, state->text_grey_brush.Get());
                    } else {
                        state->render_target->DrawTextW(status_text.c_str(), static_cast<UINT32>(status_text.size()), state->small_format.Get(), status_rect, state->text_grey_brush.Get());
                    }

                    // Card 2 inner Separator 2
                    state->render_target->DrawLine(D2D1::Point2F(250.0f, 605.0f), D2D1::Point2F(690.0f, 605.0f), state->separator_brush.Get(), 1.0f);

                    // Row 3: Theme Settings
                    D2D1_RECT_F theme_label_rect = D2D1::RectF(250.0f, 610.0f, 420.0f, 630.0f);
                    state->render_target->DrawTextW(L"应用主题", static_cast<UINT32>(wcslen(L"应用主题")), state->small_format.Get(), theme_label_rect, state->text_grey_brush.Get());

                    struct ThemeButton {
                        std::wstring_view theme;
                        const wchar_t* label;
                        int left;
                        int right;
                    };
                    const ThemeButton theme_buttons[] = {
                        { L"system", L"跟随系统", 250, 380 },
                        { L"light", L"浅色模式", 390, 520 },
                        { L"dark", L"深色模式", 530, 660 }
                    };

                    for (const auto& btn : theme_buttons) {
                        const bool selected = state->config.theme == btn.theme;
                        const bool hovered = state->mouse_pos.x >= btn.left && state->mouse_pos.x <= btn.right &&
                                             state->mouse_pos.y >= 635 && state->mouse_pos.y <= 665;
                        draw_choice_button(state, btn.left, 635, btn.right, 665, btn.label, selected, hovered);
                    }

                    D2D1_RECT_F note_rect = D2D1::RectF(230.0f, 683.0f, 700.0f, 695.0f);
                    // Just draw note text under Card 2, but make it very small and clean
                    state->render_target->DrawTextW(strings::settings_note.data(), static_cast<UINT32>(strings::settings_note.size()), state->small_format.Get(), note_rect, state->text_grey_brush.Get());

                } else if (state->active_tab == 1) {
                    std::vector<std::wstring> current_tools = split_by_comma(state->config.toolbar_order);

                    // Card 1 (Left List Box Card)
                    D2D1_RECT_F left_card_rect = D2D1::RectF(230.0f, 80.0f, 480.0f, 670.0f);
                    D2D1_ROUNDED_RECT rounded_left_card = D2D1::RoundedRect(left_card_rect, 8.0f, 8.0f);
                    state->render_target->FillRoundedRectangle(rounded_left_card, state->card_bg_brush.Get());
                    state->render_target->DrawRoundedRectangle(rounded_left_card, state->card_border_brush.Get(), 1.0f);

                    for (size_t i = 0; i < current_tools.size(); ++i) {
                        float y = 85.0f + i * 27.0f;
                        D2D1_RECT_F item_rect = D2D1::RectF(232.0f, y, 478.0f, y + 26.0f);
                        bool is_selected = (state->selected_tool_idx == static_cast<int>(i));
                        bool is_hovered = (state->mouse_pos.x >= 230 && state->mouse_pos.x <= 480 && state->mouse_pos.y >= y && state->mouse_pos.y <= y + 27.0f);
                        bool is_shown = !annotation_tool_hidden(state->config.annotation_hidden_tools, current_tools[i]);

                        if (is_selected) {
                            D2D1_ROUNDED_RECT rounded_item = D2D1::RoundedRect(item_rect, 4.0f, 4.0f);
                            state->render_target->FillRoundedRectangle(rounded_item, state->hover_bg_brush.Get());
                            
                            // Left indicator pill
                            D2D1_RECT_F indicator = D2D1::RectF(234.0f, y + 5.0f, 237.0f, y + 21.0f);
                            state->render_target->FillRoundedRectangle(D2D1::RoundedRect(indicator, 1.5f, 1.5f), state->accent_indicator_brush.Get());
                        } else if (is_hovered) {
                            D2D1_ROUNDED_RECT rounded_item = D2D1::RoundedRect(item_rect, 4.0f, 4.0f);
                            state->render_target->FillRoundedRectangle(rounded_item, state->hover_bg_brush.Get());
                        }

                        // Draw Checkbox
                        D2D1_RECT_F cb_rect = D2D1::RectF(242.0f, y + 7.0f, 254.0f, y + 19.0f);
                        D2D1_ROUNDED_RECT cb_rounded = D2D1::RoundedRect(cb_rect, 2.0f, 2.0f);
                        if (is_shown) {
                            state->render_target->FillRoundedRectangle(cb_rounded, state->blue_brush.Get());
                            // Draw white checkmark
                            float cx = 248.0f;
                            float cy = y + 13.0f;
                            state->render_target->DrawLine(D2D1::Point2F(cx - 3.0f, cy), D2D1::Point2F(cx - 1.0f, cy + 2.0f), state->accent_text_brush.Get(), 1.5f);
                            state->render_target->DrawLine(D2D1::Point2F(cx - 1.0f, cy + 2.0f), D2D1::Point2F(cx + 3.0f, cy - 2.0f), state->accent_text_brush.Get(), 1.5f);
                        } else {
                            state->render_target->DrawRoundedRectangle(cb_rounded, state->text_grey_brush.Get(), 1.0f);
                        }

                        // Tool display name
                        std::wstring displayName = get_tool_display_name(current_tools[i]);
                        ID2D1SolidColorBrush* text_brush = is_selected ? state->blue_brush.Get() : (is_shown ? state->text_white_brush.Get() : state->text_grey_brush.Get());
                        
                        D2D1_RECT_F text_rect = D2D1::RectF(262.0f, y, 420.0f, y + 26.0f);
                        state->render_target->DrawTextW(displayName.c_str(), static_cast<UINT32>(displayName.size()), state->small_format.Get(), text_rect, text_brush);

                        // Draw inline reorder buttons (Up/Down chevrons) if selected
                        if (is_selected) {
                            // Up Arrow: x: 425..445
                            bool up_arrow_hovered = (state->mouse_pos.x >= 425 && state->mouse_pos.x <= 445 && state->mouse_pos.y >= y && state->mouse_pos.y <= y + 27.0f);
                            if (i > 0) {
                                if (up_arrow_hovered) {
                                    state->render_target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(435.0f, y + 13.0f), 9.0f, 9.0f), state->hover_bg_brush.Get());
                                }
                                // Chevron Up
                                float cx = 435.0f;
                                float cy = y + 13.0f;
                                ID2D1SolidColorBrush* arrow_brush = up_arrow_hovered ? state->blue_brush.Get() : state->text_grey_brush.Get();
                                state->render_target->DrawLine(D2D1::Point2F(cx - 4.0f, cy + 2.0f), D2D1::Point2F(cx, cy - 2.0f), arrow_brush, 1.5f);
                                state->render_target->DrawLine(D2D1::Point2F(cx, cy - 2.0f), D2D1::Point2F(cx + 4.0f, cy + 2.0f), arrow_brush, 1.5f);
                            }

                            // Down Arrow: x: 450..470
                            bool down_arrow_hovered = (state->mouse_pos.x >= 450 && state->mouse_pos.x <= 470 && state->mouse_pos.y >= y && state->mouse_pos.y <= y + 27.0f);
                            if (i < current_tools.size() - 1) {
                                if (down_arrow_hovered) {
                                    state->render_target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(460.0f, y + 13.0f), 9.0f, 9.0f), state->hover_bg_brush.Get());
                                }
                                // Chevron Down
                                float cx = 460.0f;
                                float cy = y + 13.0f;
                                ID2D1SolidColorBrush* arrow_brush = down_arrow_hovered ? state->blue_brush.Get() : state->text_grey_brush.Get();
                                state->render_target->DrawLine(D2D1::Point2F(cx - 4.0f, cy - 2.0f), D2D1::Point2F(cx, cy + 2.0f), arrow_brush, 1.5f);
                                state->render_target->DrawLine(D2D1::Point2F(cx, cy + 2.0f), D2D1::Point2F(cx + 4.0f, cy - 2.0f), arrow_brush, 1.5f);
                            }
                        }
                    }

                    // Card 2 (Right Config Card)
                    D2D1_RECT_F right_card_rect = D2D1::RectF(500.0f, 80.0f, 710.0f, 670.0f);
                    D2D1_ROUNDED_RECT rounded_right_card = D2D1::RoundedRect(right_card_rect, 8.0f, 8.0f);
                    state->render_target->FillRoundedRectangle(rounded_right_card, state->card_bg_brush.Get());
                    state->render_target->DrawRoundedRectangle(rounded_right_card, state->card_border_brush.Get(), 1.0f);

                    int sel = state->selected_tool_idx;
                    if (sel >= 0 && sel < static_cast<int>(current_tools.size())) {
                        D2D1_RECT_F sub_header = D2D1::RectF(520.0f, 100.0f, 690.0f, 140.0f);
                        std::wstring sub_title = L"所选: " + get_tool_display_name(current_tools[sel]);
                        state->render_target->DrawTextW(sub_title.c_str(), static_cast<UINT32>(sub_title.size()), state->text_format.Get(), sub_header, state->text_white_brush.Get());

                        D2D1_RECT_F toggle_label = D2D1::RectF(520.0f, 160.0f, 630.0f, 190.0f);
                        state->render_target->DrawTextW(L"在工具栏中显示", static_cast<UINT32>(wcslen(L"在工具栏中显示")), state->small_format.Get(), toggle_label, state->text_white_brush.Get());

                        bool is_shown = !annotation_tool_hidden(state->config.annotation_hidden_tools, current_tools[sel]);
                        draw_switch(state, 640, 164, is_shown);

                        bool up_hovered = (state->mouse_pos.x >= 520 && state->mouse_pos.x <= 690 && state->mouse_pos.y >= 220 && state->mouse_pos.y <= 255);
                        draw_button(state, 520, 220, 690, 255, L"上移工具", up_hovered, btn_secondary);

                        bool down_hovered = (state->mouse_pos.x >= 520 && state->mouse_pos.x <= 690 && state->mouse_pos.y >= 275 && state->mouse_pos.y <= 310);
                        draw_button(state, 520, 275, 690, 310, L"下移工具", down_hovered, btn_secondary);
                    } else {
                        D2D1_RECT_F empty_rect = D2D1::RectF(520.0f, 150.0f, 690.0f, 400.0f);
                        state->render_target->DrawTextW(L"请在左侧列表中选择一个工具以调整显示顺序和启用状态。", static_cast<UINT32>(wcslen(L"请在左侧列表中选择一个工具以调整显示顺序和启用状态。")), state->small_format.Get(), empty_rect, state->text_grey_brush.Get());
                    }

                } else if (state->active_tab == 2) {
                    // Card background for shortcuts
                    D2D1_RECT_F card_rect = D2D1::RectF(230.0f, 80.0f, 710.0f, 670.0f);
                    D2D1_ROUNDED_RECT rounded_card = D2D1::RoundedRect(card_rect, 8.0f, 8.0f);
                    state->render_target->FillRoundedRectangle(rounded_card, state->card_bg_brush.Get());
                    state->render_target->DrawRoundedRectangle(rounded_card, state->card_border_brush.Get(), 1.0f);

                    // Symmetrical columns
                    const wchar_t* col1_labels[] = {
                        L"全局截图热键", L"剪贴板贴图热键", L"全局 OCR 热键", L"屏幕识字快捷键",
                        L"选择工具快捷键", L"矩形工具快捷键", L"椭圆工具快捷键", L"直线工具快捷键"
                    };
                    const wchar_t* col2_labels[] = {
                        L"箭头工具快捷键", L"画笔工具快捷键", L"马赛克快捷键", L"模糊工具快捷键",
                        L"高亮工具快捷键", L"文本工具快捷键", L"序号工具快捷键", L"橡皮擦快捷键"
                    };

                    // Draw middle vertical divider line
                    state->render_target->DrawLine(D2D1::Point2F(470.0f, 95.0f), D2D1::Point2F(470.0f, 655.0f), state->separator_brush.Get(), 1.0f);

                    // Col 1
                    for (int i = 0; i < 8; ++i) {
                        float y = 95.0f + i * 65.0f;
                        D2D1_RECT_F text_rect = D2D1::RectF(245.0f, y, 350.0f, y + 26.0f);
                        state->render_target->DrawTextW(col1_labels[i], static_cast<UINT32>(wcslen(col1_labels[i])), state->text_format.Get(), text_rect, state->text_white_brush.Get());
                        
                        std::wstring* sh = get_shortcut_ptr(state->config, i);
                        bool is_capturing = (state->capturing_idx_ == i);
                        bool is_hovered = (state->mouse_pos.x >= 355 && state->mouse_pos.x <= 455 && state->mouse_pos.y >= y && state->mouse_pos.y <= y + 26);
                        draw_hotkey_box(state, 355, static_cast<int>(y), 455, static_cast<int>(y) + 26, sh ? sh->c_str() : L"", is_capturing, is_hovered);
                    }

                    // Col 2
                    for (int i = 8; i < shortcut_count; ++i) {
                        float y = 95.0f + (i - 8) * 65.0f;
                        D2D1_RECT_F text_rect = D2D1::RectF(485.0f, y, 590.0f, y + 26.0f);
                        state->render_target->DrawTextW(col2_labels[i - 8], static_cast<UINT32>(wcslen(col2_labels[i - 8])), state->text_format.Get(), text_rect, state->text_white_brush.Get());

                        std::wstring* sh = get_shortcut_ptr(state->config, i);
                        bool is_capturing = (state->capturing_idx_ == i);
                        bool is_hovered = (state->mouse_pos.x >= 595 && state->mouse_pos.x <= 695 && state->mouse_pos.y >= y && state->mouse_pos.y <= y + 26);
                        draw_hotkey_box(state, 595, static_cast<int>(y), 695, static_cast<int>(y) + 26, sh ? sh->c_str() : L"", is_capturing, is_hovered);
                    }
                }

                // 4. Draw Footer (Dividing Line & Save/Cancel buttons)
                state->render_target->DrawLine(D2D1::Point2F(0.0f, 700.0f), D2D1::Point2F(740.0f, 700.0f), state->card_border_brush.Get(), 1.0f);

                const bool default_file =
                    _wcsicmp(state->config.default_output.c_str(), L"file") == 0;
                const bool clipboard_hovered =
                    state->mouse_pos.x >= 230 && state->mouse_pos.x <= 350 &&
                    state->mouse_pos.y >= 715 && state->mouse_pos.y <= 745;
                const bool file_hovered =
                    state->mouse_pos.x >= 360 && state->mouse_pos.x <= 490 &&
                    state->mouse_pos.y >= 715 && state->mouse_pos.y <= 745;
                draw_choice_button(state,
                                   230,
                                   715,
                                   350,
                                   745,
                                   L"默认复制",
                                   !default_file,
                                   clipboard_hovered);
                draw_choice_button(state,
                                   360,
                                   715,
                                   490,
                                   745,
                                   L"默认保存",
                                   default_file,
                                   file_hovered);

                bool save_hovered = (state->mouse_pos.x >= 500 && state->mouse_pos.x <= 600 && state->mouse_pos.y >= 715 && state->mouse_pos.y <= 745);
                draw_button(state, 500, 715, 600, 745, L"保存", save_hovered, btn_primary);

                bool cancel_hovered = (state->mouse_pos.x >= 615 && state->mouse_pos.x <= 715 && state->mouse_pos.y >= 715 && state->mouse_pos.y <= 745);
                draw_button(state, 615, 715, 715, 745, L"取消", cancel_hovered, btn_secondary);

                draw_settings_keyboard_focus(state);

                HRESULT end_hr = state->render_target->EndDraw();
                if (end_hr == D2DERR_RECREATE_TARGET) {
                    discard_resources(state);
                }
            }
            EndPaint(window, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            const float scale = settings_layout_scale(window);
            state->mouse_pos = {
                static_cast<LONG>(std::round(GET_X_LPARAM(l_param) / scale)),
                static_cast<LONG>(std::round(GET_Y_LPARAM(l_param) / scale))
            };
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            state->keyboard_focus_visible = false;
            const float scale = settings_layout_scale(window);
            POINT pt{
                static_cast<LONG>(std::round(GET_X_LPARAM(l_param) / scale)),
                static_cast<LONG>(std::round(GET_Y_LPARAM(l_param) / scale))
            };
            
            // Check close button click (x: 695 to 740, y: 0 to 50)
            if (pt.x >= 695 && pt.x <= 740 && pt.y >= 0 && pt.y <= 50) {
                PostMessageW(window, WM_CLOSE, 0, 0);
                return 0;
            }

            // Check title bar dragging (excluding the close button)
            if (pt.y >= 0 && pt.y <= 50) {
                ReleaseCapture();
                SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            
            // Check sidebar tabs
            for (int i = 0; i < 3; ++i) {
                float ty = 60.0f + i * 50.0f;
                if (pt.x >= 10 && pt.x <= 190 && pt.y >= ty && pt.y <= ty + 38.0f) {
                    state->active_tab = i;
                    state->capturing_idx_ = -1;
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
            }

            // Check footer buttons
            if (pt.y >= 715 && pt.y <= 745) {
                if (pt.x >= 230 && pt.x <= 350) {
                    state->config.default_output = L"clipboard";
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
                if (pt.x >= 360 && pt.x <= 490) {
                    state->config.default_output = L"file";
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
                if (pt.x >= 500 && pt.x <= 600) {
                    (void)accept_settings(state);
                    return 0;
                }
                if (pt.x >= 615 && pt.x <= 715) {
                    PostMessageW(window, WM_CLOSE, 0, 0);
                    return 0;
                }
            }

            // Check Content Panel
            if (state->active_tab == 0) {
                for (int i = 0; i < kGeneralSwitchCount; ++i) {
                    const int row_y = 110 + i * kGeneralRowHeight;
                    if (pt.x >= 230 && pt.x <= 710 &&
                        pt.y >= row_y && pt.y <= row_y + kGeneralRowHeight) {
                        switch (i) {
                            case 0: state->config.annotation_enabled = !state->config.annotation_enabled; break;
                            case 1:
                                state->config.ocr_enabled = !state->config.ocr_enabled;
                                if (!state->config.ocr_enabled) {
                                    state->config.global_ocr_enabled = false;
                                }
                                break;
                            case 2:
                                if (state->config.ocr_enabled) {
                                    state->config.global_ocr_enabled =
                                        !state->config.global_ocr_enabled;
                                }
                                break;
                            case 3:
                                state->config.shell_enabled = !state->config.shell_enabled;
                                break;
                            case 4:
                                if (!state->config.shell_enabled ||
                                    !toggle_tray_icon(state)) {
                                    return 0;
                                }
                                break;
                            case 5:
                                if (state->config.shell_enabled) {
                                    state->config.start_at_login =
                                        !state->config.start_at_login;
                                }
                                break;
                            case 6:
                                state->config.notifications_enabled =
                                    !state->config.notifications_enabled;
                                break;
                            case 7:
                                state->config.annotation_locked_tool =
                                    !state->config.annotation_locked_tool;
                                break;
                        }
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                }
                // Font Family button (with popup menu)
                if (pt.x >= 250 && pt.x <= 690 && pt.y >= 405 && pt.y <= 435) {
                    (void)show_font_family_menu(state);
                    return 0;
                }
                // Bold switch
                if (pt.x >= 350 && pt.x <= 394 && pt.y >= 452 && pt.y <= 474) {
                    state->config.text_font_bold = !state->config.text_font_bold;
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
                // Italic switch
                if (pt.x >= 590 && pt.x <= 634 && pt.y >= 452 && pt.y <= 474) {
                    state->config.text_font_italic = !state->config.text_font_italic;
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
                // OCR engine buttons
                if (const auto* button = hit_test_ocr_engine_button(pt)) {
                    if (!state->is_downloading) {
                        state->config.ocr_engine = std::wstring(button->engine);
                        g_ocr_download.clear_error();
                        refresh_ocr_download_state(state, true);
                        InvalidateRect(window, nullptr, TRUE);
                    }
                    return 0;
                }
                // Download button
                if (pt.x >= 530 && pt.x <= 650 && pt.y >= 565 && pt.y <= 595) {
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
                        InvalidateRect(window, nullptr, TRUE);
                    }
                    return 0;
                }
                // Theme buttons click handling (y: 635..665)
                if (pt.y >= 635 && pt.y <= 665) {
                    if (pt.x >= 250 && pt.x <= 380) {
                        state->config.theme = L"system";
                        refresh_settings_theme(state);
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                    if (pt.x >= 390 && pt.x <= 520) {
                        state->config.theme = L"light";
                        refresh_settings_theme(state);
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                    if (pt.x >= 530 && pt.x <= 660) {
                        state->config.theme = L"dark";
                        refresh_settings_theme(state);
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                }
            } else if (state->active_tab == 1) {
                std::vector<std::wstring> current_tools = split_by_comma(state->config.toolbar_order);

                // 1. Check list box item click (item height: 27px)
                if (pt.x >= 230 && pt.x <= 480 && pt.y >= 80 && pt.y <= 670) {
                    int clicked_idx = static_cast<int>((pt.y - 85) / 27);
                    if (clicked_idx >= 0 && clicked_idx < static_cast<int>(current_tools.size())) {
                        // Check if click is on Checkbox: x: 235..260
                        if (pt.x >= 235 && pt.x <= 260) {
                            toggle_hidden_tool(state->config.annotation_hidden_tools, current_tools[clicked_idx]);
                            InvalidateRect(window, nullptr, TRUE);
                            return 0;
                        }
                        
                        // Check if click is on Up Arrow (only valid if selected and clicked_idx > 0)
                        if (state->selected_tool_idx == clicked_idx && clicked_idx > 0 && pt.x >= 425 && pt.x <= 445) {
                            std::swap(current_tools[clicked_idx], current_tools[clicked_idx - 1]);
                            state->config.toolbar_order = join_by_comma(current_tools);
                            state->selected_tool_idx = clicked_idx - 1;
                            InvalidateRect(window, nullptr, TRUE);
                            return 0;
                        }
                        
                        // Check if click is on Down Arrow (only valid if selected and clicked_idx < size - 1)
                        if (state->selected_tool_idx == clicked_idx && clicked_idx < static_cast<int>(current_tools.size()) - 1 && pt.x >= 450 && pt.x <= 470) {
                            std::swap(current_tools[clicked_idx], current_tools[clicked_idx + 1]);
                            state->config.toolbar_order = join_by_comma(current_tools);
                            state->selected_tool_idx = clicked_idx + 1;
                            InvalidateRect(window, nullptr, TRUE);
                            return 0;
                        }

                        // Otherwise select the item
                        state->selected_tool_idx = clicked_idx;
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                }

                // 2. Check configuration controls on the right side
                int sel = state->selected_tool_idx;
                if (sel >= 0 && sel < static_cast<int>(current_tools.size())) {
                    // Check toggle switch: x: 640..684, y: 164..186
                    if (pt.x >= 640 && pt.x <= 684 && pt.y >= 164 && pt.y <= 186) {
                        toggle_hidden_tool(state->config.annotation_hidden_tools, current_tools[sel]);
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                    // Check "上移工具" button: x: 520..690, y: 220..255
                    if (pt.x >= 520 && pt.x <= 690 && pt.y >= 220 && pt.y <= 255) {
                        if (sel > 0) {
                            std::swap(current_tools[sel], current_tools[sel - 1]);
                            state->config.toolbar_order = join_by_comma(current_tools);
                            state->selected_tool_idx = sel - 1;
                            InvalidateRect(window, nullptr, TRUE);
                        }
                        return 0;
                    }
                    // Check "下移工具" button: x: 520..690, y: 275..310
                    if (pt.x >= 520 && pt.x <= 690 && pt.y >= 275 && pt.y <= 310) {
                        if (sel < static_cast<int>(current_tools.size()) - 1) {
                            std::swap(current_tools[sel], current_tools[sel + 1]);
                            state->config.toolbar_order = join_by_comma(current_tools);
                            state->selected_tool_idx = sel + 1;
                            InvalidateRect(window, nullptr, TRUE);
                        }
                        return 0;
                    }
                }
            } else if (state->active_tab == 2) {
                // Shortcuts tab: check every shortcut box
                // Col 1: i = 0..7
                for (int i = 0; i < 8; ++i) {
                    int sy = 95 + i * 65;
                    if (pt.x >= 355 && pt.x <= 455 && pt.y >= sy && pt.y <= sy + 26) {
                        state->capturing_idx_ = i;
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                }
                // Col 2: remaining shortcuts
                for (int i = 8; i < shortcut_count; ++i) {
                    int sy = 95 + (i - 8) * 65;
                    if (pt.x >= 595 && pt.x <= 695 && pt.y >= sy && pt.y <= sy + 26) {
                        state->capturing_idx_ = i;
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                }
            }

            // Clicked outside active hotkey box -> cancel capturing
            if (state->capturing_idx_ != -1) {
                state->capturing_idx_ = -1;
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (message == WM_KEYDOWN && w_param == VK_TAB) {
                move_settings_keyboard_focus(
                    state, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                return 0;
            }
            if (state->capturing_idx_ != -1) {
                WPARAM key = w_param;
                
                // Get modifiers
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                bool win = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;

                // Ignore pure modifier presses
                if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU || key == VK_LWIN || key == VK_RWIN) {
                    return 0;
                }

                std::wstring shortcut_str;
                if (ctrl) shortcut_str += L"Ctrl+";
                if (alt) shortcut_str += L"Alt+";
                if (shift) shortcut_str += L"Shift+";
                if (win) shortcut_str += L"Win+";

                if (key == VK_SNAPSHOT) {
                    shortcut_str += L"PrintScreen";
                } else if (key >= 'A' && key <= 'Z') {
                    shortcut_str += static_cast<wchar_t>(key);
                } else if (key >= '0' && key <= '9') {
                    shortcut_str += static_cast<wchar_t>(key);
                } else if (key >= VK_F1 && key <= VK_F24) {
                    shortcut_str += std::format(L"F{}", key - VK_F1 + 1);
                } else if (key == VK_ESCAPE) {
                    // Cancel capture
                    state->capturing_idx_ = -1;
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                } else if (key == VK_DELETE || key == VK_BACK) {
                    // Clear shortcut
                    shortcut_str = L"";
                } else {
                    // Ignore other unsupported keys
                    return 0;
                }

                std::wstring* sh = get_shortcut_ptr(state->config, state->capturing_idx_);
                if (sh) {
                    const std::wstring previous = *sh;
                    *sh = shortcut_str;
                    if (const auto validation_error = shortcut_validation_error(state->config)) {
                        *sh = previous;
                        enter_settings_modal(state);
                        MessageBoxW(window,
                                    validation_error->c_str(),
                                    L"快捷键冲突",
                                    MB_OK | MB_ICONWARNING);
                        if (!leave_settings_modal(state)) {
                            return 0;
                        }
                    }
                }
                state->capturing_idx_ = -1;
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            }
            if (message == WM_KEYDOWN &&
                (w_param == VK_RETURN || w_param == VK_SPACE)) {
                if ((l_param & (1LL << 30)) != 0) {
                    return 0;
                }
                if (state->keyboard_focus_visible && state->keyboard_focus &&
                    settings_focus_target_available(state, *state->keyboard_focus)) {
                    const SettingsFocusTarget target = *state->keyboard_focus;
                    (void)activate_settings_focus(state, target);
                    return 0;
                }
                if (w_param == VK_RETURN) {
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

        case WM_SETCURSOR: {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(window, &pt);
            
            const float scale = settings_layout_scale(window);
            pt.x = static_cast<LONG>(std::round(pt.x / scale));
            pt.y = static_cast<LONG>(std::round(pt.y / scale));
            
            bool is_hovering_interactive = false;
            
            // Check custom close button (x: 695 to 740, y: 0 to 50)
            if (pt.x >= 695 && pt.x <= 740 && pt.y >= 0 && pt.y <= 50) {
                is_hovering_interactive = true;
            }
            
            // Check tabs
            for (int i = 0; i < 3; ++i) {
                float ty = 60.0f + i * 50.0f;
                if (pt.x >= 10 && pt.x <= 190 && pt.y >= ty && pt.y <= ty + 38.0f) {
                    is_hovering_interactive = true;
                }
            }
            // Check footer buttons
            if (pt.y >= 715 && pt.y <= 745 &&
                ((pt.x >= 230 && pt.x <= 350) ||
                 (pt.x >= 360 && pt.x <= 490) ||
                 (pt.x >= 500 && pt.x <= 600) ||
                 (pt.x >= 615 && pt.x <= 715))) {
                is_hovering_interactive = true;
            }
            // Check content area elements
            if (state->active_tab == 0) {
                for (int i = 0; i < kGeneralSwitchCount; ++i) {
                    const int row_y = 110 + i * kGeneralRowHeight;
                    if (pt.x >= 230 && pt.x <= 710 &&
                        pt.y >= row_y && pt.y <= row_y + kGeneralRowHeight) {
                        if (i != 2 || state->config.ocr_enabled) {
                            is_hovering_interactive = true;
                        }
                    }
                }
                // Font Family button
                if (pt.x >= 250 && pt.x <= 690 && pt.y >= 405 && pt.y <= 435) {
                    is_hovering_interactive = true;
                }
                // Bold switch
                if (pt.x >= 350 && pt.x <= 394 && pt.y >= 452 && pt.y <= 474) {
                    is_hovering_interactive = true;
                }
                // Italic switch
                if (pt.x >= 590 && pt.x <= 634 && pt.y >= 452 && pt.y <= 474) {
                    is_hovering_interactive = true;
                }
                // OCR engine buttons
                if (hit_test_ocr_engine_button(pt)) {
                    if (!state->is_downloading) {
                        is_hovering_interactive = true;
                    }
                }
                // Download button
                if (pt.x >= 530 && pt.x <= 650 && pt.y >= 565 && pt.y <= 595) {
                    if (state->ocr_dependency.can_download &&
                        !state->is_downloading) {
                        is_hovering_interactive = true;
                    }
                }
                // Theme buttons hover check (y: 635..665)
                if (pt.y >= 635 && pt.y <= 665 && ((pt.x >= 250 && pt.x <= 380) || (pt.x >= 390 && pt.x <= 520) || (pt.x >= 530 && pt.x <= 660))) {
                    is_hovering_interactive = true;
                }
            } else if (state->active_tab == 1) {
                std::vector<std::wstring> current_tools = split_by_comma(state->config.toolbar_order);
                // Hovering list items
                for (size_t i = 0; i < current_tools.size(); ++i) {
                    float y = 85.0f + i * 27.0f;
                    if (pt.x >= 230 && pt.x <= 480 && pt.y >= y && pt.y <= y + 26.0f) {
                        is_hovering_interactive = true;
                    }
                }
                // Hovering config controls
                int sel = state->selected_tool_idx;
                if (sel >= 0 && sel < static_cast<int>(current_tools.size())) {
                    if (pt.x >= 640 && pt.x <= 684 && pt.y >= 164 && pt.y <= 186) {
                        is_hovering_interactive = true;
                    }
                    if (pt.x >= 520 && pt.x <= 690 && pt.y >= 220 && pt.y <= 255) {
                        is_hovering_interactive = true;
                    }
                    if (pt.x >= 520 && pt.x <= 690 && pt.y >= 275 && pt.y <= 310) {
                        is_hovering_interactive = true;
                    }
                }
            } else if (state->active_tab == 2) {
                // Col 1 boxes
                for (int i = 0; i < 8; ++i) {
                    int sy = 95 + i * 65;
                    if (pt.x >= 355 && pt.x <= 455 && pt.y >= sy && pt.y <= sy + 26) {
                        is_hovering_interactive = true;
                    }
                }
                // Col 2 boxes
                for (int i = 8; i < shortcut_count; ++i) {
                    int sy = 95 + (i - 8) * 65;
                    if (pt.x >= 595 && pt.x <= 695 && pt.y >= sy && pt.y <= sy + 26) {
                        is_hovering_interactive = true;
                    }
                }
            }

            if (is_hovering_interactive) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }

        case WM_DPICHANGED: {
            const RECT* suggested_rect = reinterpret_cast<const RECT*>(l_param);
            const HMONITOR monitor =
                MonitorFromRect(suggested_rect, MONITOR_DEFAULTTONEAREST);
            const RECT work_area = settings_work_area(monitor);
            const SIZE size = fitted_settings_size(HIWORD(w_param), work_area);
            const LONG minimum_x = work_area.left + kWorkAreaMargin;
            const LONG minimum_y = work_area.top + kWorkAreaMargin;
            const LONG maximum_x = work_area.right - kWorkAreaMargin - size.cx;
            const LONG maximum_y = work_area.bottom - kWorkAreaMargin - size.cy;
            const int x = static_cast<int>(std::clamp(
                suggested_rect->left, minimum_x, std::max(minimum_x, maximum_x)));
            const int y = static_cast<int>(std::clamp(
                suggested_rect->top, minimum_y, std::max(minimum_y, maximum_y)));
            SetWindowPos(window,
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

        case WM_CLOSE: {
            discard_resources(state);
            DestroyWindow(window);
            return 0;
        }

        case WM_DESTROY: {
            return 0;
        }

        case WM_NCDESTROY: {
            if (state->download_subscription != 0) {
                g_ocr_download.unsubscribe(window, state->download_subscription);
                state->download_subscription = 0;
            }
            state->window = nullptr;
            state->nc_destroyed = true;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            const LRESULT default_result = DefWindowProcW(window, message, w_param, l_param);
            restore_settings_owner(state);
            if (state->modal_depth == 0) {
                finalize_settings_state(state);
            }
            return default_result;
        }
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace

HWND show_settings_window_async(
    HWND owner,
    AppConfig config,
    SettingsWindowCompletion completion,
    SettingsWindowValidator validator) {
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
    state->palette = resolve_ui_palette(state->config.theme);
    state->is_light_theme = state->palette.light;
    state->high_contrast = state->palette.high_contrast;
    state->owner = owner;
    state->completion = std::move(completion);
    state->validator = std::move(validator);
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
