#include "settings_window.h"

#include "airshot/ocr.h"
#include "airshot/strings.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <mutex>
#include <vector>
#include <string>
#include <format>
#include <cwctype>
#include <windowsx.h>
#include <thread>
#include <filesystem>
#include <dwmapi.h>

namespace airshot {
namespace {

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
    bool is_light_theme{};
    
    // UI state
    int active_tab{0}; // 0: 常规设置, 1: 工具栏, 2: 快捷键
    int capturing_idx_{-1}; // capturing hotkey index
    int selected_tool_idx{-1};
    POINT mouse_pos{};

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
};

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

struct OcrDownloadContext {
    std::mutex mutex;
    bool is_downloading{false};
    int progress{0};
    std::wstring error;
} g_ocr_download;

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

void start_ocr_download(HWND hwnd, std::wstring_view manifest_url) {
    {
        std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
        if (g_ocr_download.is_downloading) return;
        g_ocr_download.is_downloading = true;
        g_ocr_download.progress = 0;
        g_ocr_download.error.clear();
    }
    if (IsWindow(hwnd)) {
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    std::wstring manifest_url_str(manifest_url);
    
    std::thread([manifest_url_str, hwnd]() {
        std::wstring error;
        const bool ok = download_ocr_dependencies(
            manifest_url_str,
            [hwnd](int progress) {
                {
                    std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                    g_ocr_download.progress = progress;
                }
                if (IsWindow(hwnd)) {
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            },
            &error);

        {
            std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
            g_ocr_download.is_downloading = false;
            g_ocr_download.progress = ok ? 100 : g_ocr_download.progress;
            g_ocr_download.error = ok ? std::wstring{} : error;
        }
        if (IsWindow(hwnd)) {
            InvalidateRect(hwnd, nullptr, TRUE);
        }
    }).detach();
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

bool ensure_resources(SettingsState* state) {
    const bool current_theme_is_light = should_use_light_theme(state->config.theme);
    if (state->render_target) {
        if (state->is_light_theme == current_theme_is_light) {
            return true;
        }
        discard_resources(state);
    }
    state->is_light_theme = current_theme_is_light;

    if (state->window) {
        BOOL use_dark = !state->is_light_theme;
        DwmSetWindowAttribute(state->window, 20, &use_dark, sizeof(use_dark));
        DwmSetWindowAttribute(state->window, 19, &use_dark, sizeof(use_dark));
    }

    RECT rect{};
    GetClientRect(state->window, &rect);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(rect.right - rect.left),
                                         static_cast<UINT32>(rect.bottom - rect.top));

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(state->d2d_factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(state->dwrite_factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = state->d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(state->window, size),
        state->render_target.GetAddressOf()
    );
    if (FAILED(hr)) return false;

    float dpi = static_cast<float>(GetDpiForWindow(state->window));
    if (dpi == 0.0f) {
        dpi = 96.0f;
    }
    state->render_target->SetDpi(dpi, dpi);

    // Create Brushes
    if (state->is_light_theme) {
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xF2F3F5), state->bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xF2F3F5), state->sidebar_bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x1F2329), state->text_white_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x646A73), state->text_grey_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x0066FF), state->blue_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x3385FF), state->hover_blue_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xDEE0E3), state->border_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xE5E7EB), state->active_tab_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), state->control_bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xD2D6DC), state->switch_track_off_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), state->card_bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xE5E7EB), state->card_border_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xF0F1F3), state->separator_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xF5F7FA), state->hover_bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xD2D6DC), state->cancel_btn_border_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x0066FF), state->accent_indicator_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xD0D3D8), state->keycap_shadow_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x0066FF, 0.15f), state->switch_glow_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xFFEAEA), state->close_btn_hover_bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xE81123), state->red_brush.GetAddressOf());
    } else {
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(22.0f / 255.0f, 23.0f / 255.0f, 28.0f / 255.0f), state->bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(22.0f / 255.0f, 23.0f / 255.0f, 28.0f / 255.0f), state->sidebar_bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(240.0f / 255.0f, 240.0f / 255.0f, 240.0f / 255.0f), state->text_white_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(150.0f / 255.0f, 160.0f / 255.0f, 175.0f / 255.0f), state->text_grey_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0.0f / 255.0f, 102.0f / 255.0f, 255.0f / 255.0f), state->blue_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(51.0f / 255.0f, 136.0f / 255.0f, 255.0f / 255.0f), state->hover_blue_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(45.0f / 255.0f, 48.0f / 255.0f, 56.0f / 255.0f), state->border_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(35.0f / 255.0f, 37.0f / 255.0f, 44.0f / 255.0f), state->active_tab_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(28.0f / 255.0f, 30.0f / 255.0f, 34.0f / 255.0f), state->control_bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(76.0f / 255.0f, 82.0f / 255.0f, 93.0f / 255.0f), state->switch_track_off_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(30.0f / 255.0f, 32.0f / 255.0f, 38.0f / 255.0f), state->card_bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(45.0f / 255.0f, 48.0f / 255.0f, 56.0f / 255.0f), state->card_border_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(40.0f / 255.0f, 43.0f / 255.0f, 50.0f / 255.0f), state->separator_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(38.0f / 255.0f, 41.0f / 255.0f, 48.0f / 255.0f), state->hover_bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(60.0f / 255.0f, 64.0f / 255.0f, 72.0f / 255.0f), state->cancel_btn_border_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(51.0f / 255.0f, 136.0f / 255.0f, 255.0f / 255.0f), state->accent_indicator_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(20.0f / 255.0f, 22.0f / 255.0f, 26.0f / 255.0f), state->keycap_shadow_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0.0f / 255.0f, 102.0f / 255.0f, 255.0f / 255.0f, 0.25f), state->switch_glow_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0x401515), state->close_btn_hover_bg_brush.GetAddressOf());
        state->render_target->CreateSolidColorBrush(D2D1::ColorF(0xE81123), state->red_brush.GetAddressOf());
    }

    // Create Text Formats
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-CN", state->title_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", state->text_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-CN", state->small_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", state->hotkey_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", state->btn_text_format.GetAddressOf());

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
    ID2D1SolidColorBrush* thumb_brush = state->is_light_theme ? state->control_bg_brush.Get() : state->text_white_brush.Get();
    state->render_target->FillEllipse(thumb, thumb_brush);
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
        ID2D1SolidColorBrush* text_brush = state->is_light_theme ? state->control_bg_brush.Get() : state->text_white_brush.Get();
        state->render_target->DrawTextW(label, static_cast<UINT32>(wcslen(label)), state->btn_text_format.Get(), rect, text_brush);
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
        ID2D1SolidColorBrush* text_brush = state->is_light_theme ? state->control_bg_brush.Get() : state->text_white_brush.Get();
        state->render_target->DrawTextW(label, static_cast<UINT32>(wcslen(label)), state->btn_text_format.Get(), rect, text_brush);
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
        state->render_target->DrawTextW(hotkey_str, static_cast<UINT32>(wcslen(hotkey_str)), state->hotkey_format.Get(), rect, state->text_white_brush.Get());
    }
}

LRESULT CALLBACK settings_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<SettingsState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
        case WM_CREATE: {
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(window, &ps);
            
            if (ensure_resources(state)) {
                state->render_target->BeginDraw();
                
                // Clear background
                if (state->is_light_theme) {
                    state->render_target->Clear(D2D1::ColorF(0xF2F3F5));
                } else {
                    state->render_target->Clear(D2D1::ColorF(22.0f / 255.0f, 23.0f / 255.0f, 28.0f / 255.0f));
                }

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
                        L"运行系统托盘 (System Tray Icon)",
                        L"开机自动启动 (Start at Login)",
                        L"启用提示通知 (Show Notifications)",
                        L"标注后保持当前工具 (Keep tool active)"
                    };
                    bool values[] = {
                        state->config.annotation_enabled,
                        state->config.ocr_enabled,
                        state->config.shell_enabled,
                        state->config.start_at_login,
                        state->config.notifications_enabled,
                        state->config.annotation_locked_tool
                    };

                    for (int i = 0; i < 6; ++i) {
                        float row_y = 110.0f + i * 40.0f;
                        
                        // Row hover background
                        D2D1_RECT_F row_rect = D2D1::RectF(231.0f, row_y + 1.0f, 709.0f, row_y + 39.0f);
                        bool is_row_hovered = (state->mouse_pos.x >= 230 && state->mouse_pos.x <= 710 && state->mouse_pos.y >= row_y && state->mouse_pos.y <= row_y + 40.0f);
                        if (is_row_hovered) {
                            D2D1_ROUNDED_RECT rounded_row = D2D1::RoundedRect(
                                row_rect, 
                                (i == 0 || i == 5) ? 7.0f : 0.0f, 
                                (i == 0 || i == 5) ? 7.0f : 0.0f
                            );
                            state->render_target->FillRoundedRectangle(rounded_row, state->hover_bg_brush.Get());
                        }

                        // Label
                        D2D1_RECT_F text_rect = D2D1::RectF(250.0f, row_y, 620.0f, row_y + 40.0f);
                        state->render_target->DrawTextW(labels[i], static_cast<UINT32>(wcslen(labels[i])), state->text_format.Get(), text_rect, state->text_white_brush.Get());
                        
                        // Switch
                        draw_switch(state, 640, static_cast<int>(row_y) + 9, values[i]);

                        // Separator line
                        if (i < 5) {
                            state->render_target->DrawLine(
                                D2D1::Point2F(250.0f, row_y + 40.0f), 
                                D2D1::Point2F(690.0f, row_y + 40.0f), 
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

                    // Row 0: Font Family & Reset serial
                    std::wstring font_desc = L"文本字体: " + state->config.text_font_family;
                    bool font_hovered = (state->mouse_pos.x >= 250 && state->mouse_pos.x <= 450 && state->mouse_pos.y >= 405 && state->mouse_pos.y <= 435);
                    draw_button(state, 250, 405, 450, 435, font_desc.c_str(), font_hovered, btn_secondary);

                    bool reset_hovered = (state->mouse_pos.x >= 530 && state->mouse_pos.x <= 690 && state->mouse_pos.y >= 405 && state->mouse_pos.y <= 435);
                    draw_button(state, 530, 405, 690, 435, L"重置序号计数", reset_hovered, btn_secondary);

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

                    bool is_downloading = false;
                    int progress = 0;
                    std::wstring dl_error;
                    {
                        std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                        is_downloading = g_ocr_download.is_downloading;
                        progress = g_ocr_download.progress;
                        dl_error = g_ocr_download.error;
                    }

                    OcrDependencyStatus dependency_status = ocr_dependency_status(state->config.ocr_engine);
                    std::wstring status_text = dependency_status.message;
                    bool needs_download = dependency_status.can_download;

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
                            state->render_target->DrawLine(D2D1::Point2F(cx - 3.0f, cy), D2D1::Point2F(cx - 1.0f, cy + 2.0f), state->is_light_theme ? state->control_bg_brush.Get() : state->text_white_brush.Get(), 1.5f);
                            state->render_target->DrawLine(D2D1::Point2F(cx - 1.0f, cy + 2.0f), D2D1::Point2F(cx + 3.0f, cy - 2.0f), state->is_light_theme ? state->control_bg_brush.Get() : state->text_white_brush.Get(), 1.5f);
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
                        L"全局截图热键", L"全局 OCR 热键", L"屏幕识字快捷键", L"选择工具快捷键",
                        L"矩形工具快捷键", L"椭圆工具快捷键", L"直线工具快捷键", L"箭头工具快捷键"
                    };
                    const wchar_t* col2_labels[] = {
                        L"画笔工具快捷键", L"马赛克快捷键", L"模糊工具快捷键", L"高亮工具快捷键",
                        L"文本工具快捷键", L"序号工具快捷键", L"橡皮擦快捷键"
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
                    for (int i = 8; i < 15; ++i) {
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

                bool save_hovered = (state->mouse_pos.x >= 500 && state->mouse_pos.x <= 600 && state->mouse_pos.y >= 715 && state->mouse_pos.y <= 745);
                draw_button(state, 500, 715, 600, 745, L"保存", save_hovered, btn_primary);

                bool cancel_hovered = (state->mouse_pos.x >= 615 && state->mouse_pos.x <= 715 && state->mouse_pos.y >= 715 && state->mouse_pos.y <= 745);
                draw_button(state, 615, 715, 715, 745, L"取消", cancel_hovered, btn_secondary);

                HRESULT end_hr = state->render_target->EndDraw();
                if (end_hr == D2DERR_RECREATE_TARGET) {
                    discard_resources(state);
                }
            }
            EndPaint(window, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
            if (scale <= 0.0f) scale = 1.0f;
            state->mouse_pos = {
                static_cast<LONG>(std::round(GET_X_LPARAM(l_param) / scale)),
                static_cast<LONG>(std::round(GET_Y_LPARAM(l_param) / scale))
            };
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
            if (scale <= 0.0f) scale = 1.0f;
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
                if (pt.x >= 500 && pt.x <= 600) {
                    state->accepted = true;
                    PostMessageW(window, WM_CLOSE, 0, 0);
                    return 0;
                }
                if (pt.x >= 615 && pt.x <= 715) {
                    PostMessageW(window, WM_CLOSE, 0, 0);
                    return 0;
                }
            }

            // Check Content Panel
            if (state->active_tab == 0) {
                // Switches y ranges: 110 + i * 40 + 9
                for (int i = 0; i < 6; ++i) {
                    int sy = 110 + i * 40 + 9;
                    if (pt.x >= 640 && pt.x <= 684 && pt.y >= sy && pt.y <= sy + 22) {
                        switch (i) {
                            case 0: state->config.annotation_enabled = !state->config.annotation_enabled; break;
                            case 1: state->config.ocr_enabled = !state->config.ocr_enabled; break;
                            case 2: state->config.shell_enabled = !state->config.shell_enabled; break;
                            case 3: state->config.start_at_login = !state->config.start_at_login; break;
                            case 4: state->config.notifications_enabled = !state->config.notifications_enabled; break;
                            case 5: state->config.annotation_locked_tool = !state->config.annotation_locked_tool; break;
                        }
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                }
                // Font Family button (with popup menu)
                if (pt.x >= 250 && pt.x <= 450 && pt.y >= 405 && pt.y <= 435) {
                    HMENU menu = CreatePopupMenu();
                    const wchar_t* fonts[] = {
                        L"Microsoft YaHei",
                        L"Consolas",
                        L"SimSun",
                        L"SimHei",
                        L"KaiTi",
                        L"Arial",
                        L"Segoe UI"
                    };
                    for (int i = 0; i < 7; ++i) {
                        UINT flags = MF_STRING;
                        if (state->config.text_font_family == fonts[i]) {
                            flags |= MF_CHECKED;
                        }
                        AppendMenuW(menu, flags, 1000 + i, fonts[i]);
                    }
                    
                    POINT screen_pt{ 250, 435 };
                    ClientToScreen(window, &screen_pt);
                    
                    int selection = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, screen_pt.x, screen_pt.y, 0, window, nullptr);
                    DestroyMenu(menu);
                    
                    if (selection >= 1000 && selection < 1007) {
                        state->config.text_font_family = fonts[selection - 1000];
                        InvalidateRect(window, nullptr, TRUE);
                    }
                    return 0;
                }
                // Reset serial button
                if (pt.x >= 530 && pt.x <= 690 && pt.y >= 405 && pt.y <= 435) {
                    state->config.annotation_next_serial = 1;
                    MessageBoxW(window, L"标注序号计数器已成功重置为 1。", L"设置", MB_OK | MB_ICONINFORMATION);
                    InvalidateRect(window, nullptr, TRUE);
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
                    bool is_downloading = false;
                    {
                        std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                        is_downloading = g_ocr_download.is_downloading;
                    }
                    if (!is_downloading) {
                        state->config.ocr_engine = std::wstring(button->engine);
                        {
                            std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                            g_ocr_download.error.clear();
                        }
                        InvalidateRect(window, nullptr, TRUE);
                    }
                    return 0;
                }
                // Download button
                if (pt.x >= 530 && pt.x <= 650 && pt.y >= 565 && pt.y <= 595) {
                    bool is_downloading = false;
                    {
                        std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                        is_downloading = g_ocr_download.is_downloading;
                    }
                    const OcrDependencyStatus status = ocr_dependency_status(state->config.ocr_engine);
                    if (status.can_download && !is_downloading) {
                        start_ocr_download(window, state->config.ocr_download_url);
                        InvalidateRect(window, nullptr, TRUE);
                    }
                    return 0;
                }
                // Theme buttons click handling (y: 635..665)
                if (pt.y >= 635 && pt.y <= 665) {
                    if (pt.x >= 250 && pt.x <= 380) {
                        state->config.theme = L"system";
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                    if (pt.x >= 390 && pt.x <= 520) {
                        state->config.theme = L"light";
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                    if (pt.x >= 530 && pt.x <= 660) {
                        state->config.theme = L"dark";
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
                // Shortcuts tab: check 15 boxes
                // Col 1: i = 0..7
                for (int i = 0; i < 8; ++i) {
                    int sy = 95 + i * 65;
                    if (pt.x >= 355 && pt.x <= 455 && pt.y >= sy && pt.y <= sy + 26) {
                        state->capturing_idx_ = i;
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                }
                // Col 2: i = 8..14
                for (int i = 8; i < 15; ++i) {
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
                    *sh = shortcut_str;
                }
                state->capturing_idx_ = -1;
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            }
            break;
        }

        case WM_SETCURSOR: {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(window, &pt);
            
            float scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
            if (scale <= 0.0f) scale = 1.0f;
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
            if (pt.y >= 715 && pt.y <= 745 && ((pt.x >= 500 && pt.x <= 600) || (pt.x >= 615 && pt.x <= 715))) {
                is_hovering_interactive = true;
            }
            // Check content area elements
            if (state->active_tab == 0) {
                for (int i = 0; i < 6; ++i) {
                    int sy = 110 + i * 40 + 9;
                    if (pt.x >= 640 && pt.x <= 684 && pt.y >= sy && pt.y <= sy + 22) {
                        is_hovering_interactive = true;
                    }
                }
                // Font Family button
                if (pt.x >= 250 && pt.x <= 450 && pt.y >= 405 && pt.y <= 435) {
                    is_hovering_interactive = true;
                }
                // Reset serial button
                if (pt.x >= 530 && pt.x <= 690 && pt.y >= 405 && pt.y <= 435) {
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
                    bool is_downloading = false;
                    {
                        std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                        is_downloading = g_ocr_download.is_downloading;
                    }
                    if (!is_downloading) {
                        is_hovering_interactive = true;
                    }
                }
                // Download button
                if (pt.x >= 530 && pt.x <= 650 && pt.y >= 565 && pt.y <= 595) {
                    bool is_downloading = false;
                    {
                        std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                        is_downloading = g_ocr_download.is_downloading;
                    }
                    const OcrDependencyStatus status = ocr_dependency_status(state->config.ocr_engine);
                    if (status.can_download && !is_downloading) {
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
                for (int i = 8; i < 15; ++i) {
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
            SetWindowPos(window, nullptr,
                         suggested_rect->left, suggested_rect->top,
                         suggested_rect->right - suggested_rect->left,
                         suggested_rect->bottom - suggested_rect->top,
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
            PostMessageW(nullptr, WM_NULL, 0, 0);
            return 0;
        }
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace

bool show_settings_window(HWND owner, AppConfig& config) {
    static std::once_flag class_flag;
    std::call_once(class_flag, [] {
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.style = CS_DROPSHADOW;
        window_class.lpfnWndProc = settings_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = CreateSolidBrush(RGB(18, 19, 22));
        window_class.lpszClassName = L"AirScreenshot.Settings";
        RegisterClassExW(&window_class);
    });

    SettingsState state;
    state.config = config;
    constexpr int window_width = 740;
    constexpr int window_height = 760;
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    if (dpi == 0) {
        dpi = 96;
    }
    float scale = static_cast<float>(dpi) / 96.0f;
    const int scaled_width = static_cast<int>(std::round(window_width * scale));
    const int scaled_height = static_cast<int>(std::round(window_height * scale));

    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    RECT work_area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO monitor_info{sizeof(monitor_info)};
    if (HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        work_area = monitor_info.rcWork;
    }
    auto clamp_position = [](LONG value, LONG size, LONG minimum, LONG maximum) {
        if (size >= maximum - minimum) {
            return static_cast<int>(minimum);
        }
        return static_cast<int>(std::clamp(value, minimum, maximum - size));
    };
    const int x = clamp_position(owner_rect.left + 40L, scaled_width, work_area.left + 8L, work_area.right - 8L);
    const int y = clamp_position(owner_rect.top + 40L, scaled_height, work_area.top + 8L, work_area.bottom - 8L);
    
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.Settings",
                                  strings::settings_title.data(),
                                  WS_POPUP | WS_SYSMENU,
                                  x,
                                  y,
                                  scaled_width,
                                  scaled_height,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  &state);
    if (!window) {
        return false;
    }

    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(window, &margins);

    BOOL use_dark = !should_use_light_theme(config.theme);
    DwmSetWindowAttribute(window, 20, &use_dark, sizeof(use_dark));
    DwmSetWindowAttribute(window, 19, &use_dark, sizeof(use_dark));
    DWORD corner_preference = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(window, 33, &corner_preference, sizeof(corner_preference));
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.accepted) {
        config = std::move(state.config);
        return true;
    }
    return false;
}

} // namespace airshot
