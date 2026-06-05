#include "settings_window.h"

#include "airshot/strings.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <mutex>
#include <vector>
#include <string>
#include <format>
#include <cwctype>
#include <windowsx.h>
#include <thread>
#include <filesystem>
#include <urlmon.h>

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

    // Text Formats
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> small_format;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> hotkey_format;

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

std::wstring get_wechat_install_path() {
    HKEY hKey;
    std::wstring install_path;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Tencent\\WeChat", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[512]{};
        DWORD type = REG_SZ;
        DWORD size = sizeof(buf);
        if (RegQueryValueExW(hKey, L"InstallPath", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS) {
            install_path = buf;
        }
        RegCloseKey(hKey);
    }
    return install_path;
}

std::wstring find_wechat_ocr_exe_dir() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    if (appdata) {
        std::filesystem::path ocr_root = std::filesystem::path(appdata) / L"Tencent" / L"WeChat" / L"XPlugin" / L"Plugins" / L"ocr";
        if (std::filesystem::exists(ocr_root)) {
            std::filesystem::path best_dir;
            int best_ver = -1;
            for (const auto& entry : std::filesystem::directory_iterator(ocr_root)) {
                if (entry.is_directory()) {
                    std::filesystem::path exe_path = entry.path() / L"WeChatOCR.exe";
                    if (std::filesystem::exists(exe_path)) {
                        try {
                            int ver = std::stoi(entry.path().filename().wstring());
                            if (ver > best_ver) {
                                best_ver = ver;
                                best_dir = entry.path();
                            }
                        } catch (...) {
                            if (best_ver == -1) {
                                best_dir = entry.path();
                            }
                        }
                    }
                }
            }
            return best_dir.wstring();
        }
    }
    return L"";
}

bool check_dependency_exists(const wchar_t* rel_path) {
    // 1. Check current exe folder
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::filesystem::path local_path = std::filesystem::path(exe_path).parent_path() / L"ocr_onnx" / rel_path;
    if (std::filesystem::exists(local_path)) {
        return true;
    }
    // 2. Check AppData folder
    std::filesystem::path app_data_path = config_directory() / L"ocr_onnx" / rel_path;
    if (std::filesystem::exists(app_data_path)) {
        return true;
    }
    return false;
}

class DownloadProgressCallback : public IBindStatusCallback {
public:
    DownloadProgressCallback(HWND hwnd, int* progress_ptr)
        : hwnd_(hwnd), progress_ptr_(progress_ptr), ref_count_(1) {}

    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *ppvObject = static_cast<IBindStatusCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG __stdcall AddRef() override {
        return InterlockedIncrement(&ref_count_);
    }

    ULONG __stdcall Release() override {
        ULONG count = InterlockedDecrement(&ref_count_);
        if (count == 0) {
            delete this;
            return 0;
        }
        return count;
    }

    HRESULT __stdcall OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT __stdcall GetPriority(LONG*) override { return S_OK; }
    HRESULT __stdcall OnLowResource(DWORD) override { return S_OK; }
    HRESULT __stdcall OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT __stdcall GetBindInfo(DWORD*, BINDINFO*) override { return S_OK; }
    HRESULT __stdcall OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT __stdcall OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }

    HRESULT __stdcall OnProgress(ULONG ulProgress, ULONG ulProgressMax, ULONG /*ulStatusCode*/, LPCWSTR /*szStatusText*/) override {
        if (ulProgressMax > 0) {
            int new_progress = static_cast<int>((static_cast<double>(ulProgress) / ulProgressMax) * 100.0);
            {
                std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                g_ocr_download.progress = new_progress;
            }
            if (IsWindow(hwnd_)) {
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
        }
        return S_OK;
    }

private:
    HWND hwnd_;
    int* progress_ptr_;
    ULONG ref_count_;
};

void start_ocr_download(HWND hwnd, std::wstring_view url) {
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

    std::wstring url_str(url);
    
    std::thread([url_str, hwnd]() {
        std::filesystem::path data_dir = config_directory();
        std::filesystem::path zip_path = data_dir / L"ocr_dependency.zip";
        std::filesystem::path ocr_dir = data_dir / L"ocr_onnx";

        std::filesystem::create_directories(data_dir);

        DownloadProgressCallback* callback = new DownloadProgressCallback(hwnd, &g_ocr_download.progress);
        HRESULT hr = URLDownloadToFileW(nullptr, url_str.c_str(), zip_path.c_str(), 0, callback);
        callback->Release();
        
        bool download_failed = FAILED(hr);
        
        if (download_failed) {
            {
                std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                g_ocr_download.is_downloading = false;
                g_ocr_download.error = L"下载失败。";
            }
            std::error_code ec;
            std::filesystem::remove(zip_path, ec);
            if (IsWindow(hwnd)) {
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return;
        }

        std::filesystem::create_directories(ocr_dir);
        
        std::wstring cmd = L"tar.exe -xf \"" + zip_path.wstring() + L"\" -C \"" + ocr_dir.wstring() + L"\"";
        
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        
        BOOL success = CreateProcessW(
            nullptr,
            cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        );
        
        if (success) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        std::error_code ec;
        std::filesystem::remove(zip_path, ec);

        {
            std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
            g_ocr_download.is_downloading = false;
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

bool ensure_resources(SettingsState* state) {
    if (state->render_target) {
        return true;
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
    state->render_target->CreateSolidColorBrush(D2D1::ColorF(30.0f / 255.0f, 35.0f / 255.0f, 43.0f / 255.0f), state->bg_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(D2D1::ColorF(24.0f / 255.0f, 28.0f / 255.0f, 34.0f / 255.0f), state->sidebar_bg_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(D2D1::ColorF(240.0f / 255.0f, 240.0f / 255.0f, 240.0f / 255.0f), state->text_white_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(D2D1::ColorF(150.0f / 255.0f, 160.0f / 255.0f, 175.0f / 255.0f), state->text_grey_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(D2D1::ColorF(22.0f / 255.0f, 119.0f / 255.0f, 255.0f / 255.0f), state->blue_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(D2D1::ColorF(64.0f / 255.0f, 150.0f / 255.0f, 255.0f / 255.0f), state->hover_blue_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(D2D1::ColorF(45.0f / 255.0f, 52.0f / 255.0f, 64.0f / 255.0f), state->border_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(D2D1::ColorF(36.0f / 255.0f, 42.0f / 255.0f, 51.0f / 255.0f), state->active_tab_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(D2D1::ColorF(38.0f / 255.0f, 44.0f / 255.0f, 54.0f / 255.0f), state->control_bg_brush.GetAddressOf());
    state->render_target->CreateSolidColorBrush(D2D1::ColorF(76.0f / 255.0f, 82.0f / 255.0f, 93.0f / 255.0f), state->switch_track_off_brush.GetAddressOf());

    // Create Text Formats
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-CN", state->title_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", state->text_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-CN", state->small_format.GetAddressOf());
    state->dwrite_factory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", state->hotkey_format.GetAddressOf());

    state->title_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->small_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->hotkey_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    state->hotkey_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    return true;
}

void discard_resources(SettingsState* state) {
    state->render_target.Reset();
    state->d2d_factory.Reset();
    state->dwrite_factory.Reset();
}

void draw_switch(SettingsState* state, int x, int y, bool is_on) {
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
    state->render_target->FillEllipse(thumb, state->text_white_brush.Get());
}

void draw_button(SettingsState* state, int x1, int y1, int x2, int y2, const wchar_t* label, bool is_hovered) {
    D2D1_RECT_F rect = D2D1::RectF(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(x2), static_cast<float>(y2));
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, 4.0f, 4.0f);
    
    if (is_hovered) {
        state->render_target->FillRoundedRectangle(rounded, state->hover_blue_brush.Get());
    } else {
        state->render_target->FillRoundedRectangle(rounded, state->blue_brush.Get());
    }
    state->render_target->DrawRoundedRectangle(rounded, state->border_brush.Get(), 1.0f);

    state->render_target->DrawTextW(label, static_cast<UINT32>(wcslen(label)), state->text_format.Get(), rect, state->text_white_brush.Get());
}

void draw_hotkey_box(SettingsState* state, int x1, int y1, int x2, int y2, const wchar_t* hotkey_str, bool is_capturing, bool is_hovered) {
    D2D1_RECT_F rect = D2D1::RectF(static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(x2), static_cast<float>(y2));
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, 4.0f, 4.0f);

    state->render_target->FillRoundedRectangle(rounded, state->control_bg_brush.Get());
    
    if (is_capturing) {
        state->render_target->DrawRoundedRectangle(rounded, state->blue_brush.Get(), 1.5f);
        state->render_target->DrawTextW(L"按下按键...", 7, state->hotkey_format.Get(), rect, state->blue_brush.Get());
    } else {
        if (is_hovered) {
            state->render_target->DrawRoundedRectangle(rounded, state->hover_blue_brush.Get(), 1.0f);
        } else {
            state->render_target->DrawRoundedRectangle(rounded, state->border_brush.Get(), 1.0f);
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
                state->render_target->Clear(D2D1::ColorF(30.0f / 255.0f, 35.0f / 255.0f, 43.0f / 255.0f));

                // 1. Draw Left Sidebar background
                D2D1_RECT_F sidebar_rect = D2D1::RectF(0.0f, 0.0f, 200.0f, 620.0f);
                state->render_target->FillRectangle(sidebar_rect, state->sidebar_bg_brush.Get());
                state->render_target->DrawLine(D2D1::Point2F(200.0f, 0.0f), D2D1::Point2F(200.0f, 620.0f), state->border_brush.Get(), 1.0f);

                // Sidebar Tab items
                const wchar_t* tab_labels[] = { L"常规设置", L"工具栏设置", L"全局与工具快捷键" };
                for (int i = 0; i < 3; ++i) {
                    float ty = 60.0f + i * 60.0f;
                    D2D1_RECT_F tab_rect = D2D1::RectF(10.0f, ty, 190.0f, ty + 44.0f);
                    
                    if (state->active_tab == i) {
                        state->render_target->FillRoundedRectangle(D2D1::RoundedRect(tab_rect, 4.0f, 4.0f), state->active_tab_brush.Get());
                        state->render_target->DrawTextW(tab_labels[i], static_cast<UINT32>(wcslen(tab_labels[i])), state->text_format.Get(), tab_rect, state->blue_brush.Get());
                    } else {
                        bool is_hovered = (state->mouse_pos.x >= 10 && state->mouse_pos.x <= 190 && state->mouse_pos.y >= ty && state->mouse_pos.y <= ty + 44);
                        if (is_hovered) {
                            state->render_target->FillRoundedRectangle(D2D1::RoundedRect(tab_rect, 4.0f, 4.0f), state->control_bg_brush.Get());
                        }
                        state->render_target->DrawTextW(tab_labels[i], static_cast<UINT32>(wcslen(tab_labels[i])), state->text_format.Get(), tab_rect, state->text_grey_brush.Get());
                    }
                }

                // 2. Draw Right Panel header based on active tab
                D2D1_RECT_F header_rect = D2D1::RectF(230.0f, 20.0f, 700.0f, 55.0f);
                const wchar_t* tab_titles[] = { L"常规功能配置", L"工具显示隐藏设置", L"快捷键配置与按键绑定" };
                state->render_target->DrawTextW(tab_titles[state->active_tab], static_cast<UINT32>(wcslen(tab_titles[state->active_tab])), state->title_format.Get(), header_rect, state->text_white_brush.Get());
                state->render_target->DrawLine(D2D1::Point2F(230.0f, 55.0f), D2D1::Point2F(700.0f, 55.0f), state->border_brush.Get(), 1.0f);

                // 3. Draw Content Panel
                if (state->active_tab == 0) {
                    // General settings list
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
                        float y = 80.0f + i * 55.0f;
                        D2D1_RECT_F text_rect = D2D1::RectF(230.0f, y + 10.0f, 620.0f, y + 32.0f);
                        state->render_target->DrawTextW(labels[i], static_cast<UINT32>(wcslen(labels[i])), state->text_format.Get(), text_rect, state->text_white_brush.Get());
                        draw_switch(state, 650, static_cast<int>(y) + 10, values[i]);
                    }

                    // Reset serial & Text Font Family
                    D2D1_RECT_F font_label_rect = D2D1::RectF(230.0f, 415.0f, 400.0f, 445.0f);
                    std::wstring font_desc = L"文本字体: " + state->config.text_font_family;
                    bool font_hovered = (state->mouse_pos.x >= 230 && state->mouse_pos.x <= 420 && state->mouse_pos.y >= 415 && state->mouse_pos.y <= 445);
                    draw_button(state, 230, 415, 420, 445, font_desc.c_str(), font_hovered);

                    bool reset_hovered = (state->mouse_pos.x >= 550 && state->mouse_pos.x <= 694 && state->mouse_pos.y >= 415 && state->mouse_pos.y <= 445);
                    draw_button(state, 550, 415, 694, 445, L"重置序号计数", reset_hovered);

                    // Text bold & italic
                    D2D1_RECT_F bold_label_rect = D2D1::RectF(230.0f, 465.0f, 330.0f, 495.0f);
                    state->render_target->DrawTextW(L"文本加粗 (Bold)", static_cast<UINT32>(wcslen(L"文本加粗 (Bold)")), state->small_format.Get(), bold_label_rect, state->text_white_brush.Get());
                    draw_switch(state, 350, 465, state->config.text_font_bold);

                    D2D1_RECT_F italic_label_rect = D2D1::RectF(480.0f, 465.0f, 580.0f, 495.0f);
                    state->render_target->DrawTextW(L"文本倾斜 (Italic)", static_cast<UINT32>(wcslen(L"文本倾斜 (Italic)")), state->small_format.Get(), italic_label_rect, state->text_white_brush.Get());
                    draw_switch(state, 600, 465, state->config.text_font_italic);

                    // OCR Engine selection
                    std::wstring ocr_desc;
                    if (state->config.ocr_engine == 0) ocr_desc = L"OCR 引擎: 系统自带";
                    else if (state->config.ocr_engine == 1) ocr_desc = L"OCR 引擎: 微信 OCR";
                    else ocr_desc = L"OCR 引擎: 本地高精度";
                    bool ocr_hovered = (state->mouse_pos.x >= 230 && state->mouse_pos.x <= 420 && state->mouse_pos.y >= 515 && state->mouse_pos.y <= 545);
                    draw_button(state, 230, 515, 420, 545, ocr_desc.c_str(), ocr_hovered);

                    // Check status
                    std::wstring status_text;
                    bool needs_download = false;
                    bool is_downloading = false;
                    int progress = 0;
                    std::wstring dl_error;

                    {
                        std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                        is_downloading = g_ocr_download.is_downloading;
                        progress = g_ocr_download.progress;
                        dl_error = g_ocr_download.error;
                    }

                    if (is_downloading) {
                        status_text = std::format(L"正在下载... {}%", progress);
                    } else if (!dl_error.empty()) {
                        status_text = dl_error;
                    } else {
                        if (state->config.ocr_engine == 0) {
                            status_text = L"状态: 就绪";
                        } else if (state->config.ocr_engine == 1) {
                            std::wstring wechat_dir = get_wechat_install_path();
                            std::wstring ocr_dir = find_wechat_ocr_exe_dir();
                            bool wechat_installed = !wechat_dir.empty() && !ocr_dir.empty();
                            bool dll_exists = check_dependency_exists(L"wechat_ocr_api.dll");
                            if (!wechat_installed) {
                                status_text = L"状态: 未找到微信";
                            } else if (!dll_exists) {
                                status_text = L"状态: 未下载依赖";
                                needs_download = true;
                            } else {
                                status_text = L"状态: 就绪";
                            }
                        } else if (state->config.ocr_engine == 2) {
                            bool files_exist = check_dependency_exists(L"rapidocr_api.dll") &&
                                               check_dependency_exists(L"onnxruntime.dll") &&
                                               check_dependency_exists(L"models/ch_PP-OCRv4_det_infer.onnx");
                            if (!files_exist) {
                                status_text = L"状态: 未下载依赖";
                                needs_download = true;
                            } else {
                                status_text = L"状态: 就绪";
                            }
                        }
                    }

                    D2D1_RECT_F status_rect = D2D1::RectF(440.0f, 520.0f, 580.0f, 545.0f);
                    if (is_downloading) {
                        // Draw progress bar
                        D2D1_RECT_F progress_track = D2D1::RectF(440.0f, 522.0f, 640.0f, 538.0f);
                        state->render_target->FillRectangle(progress_track, state->control_bg_brush.Get());
                        state->render_target->DrawRectangle(progress_track, state->border_brush.Get(), 1.0f);

                        float fill_width = 200.0f * (static_cast<float>(progress) / 100.0f);
                        D2D1_RECT_F progress_fill = D2D1::RectF(440.0f, 522.0f, 440.0f + fill_width, 538.0f);
                        state->render_target->FillRectangle(progress_fill, state->blue_brush.Get());

                        D2D1_RECT_F text_rect = D2D1::RectF(650.0f, 517.0f, 730.0f, 545.0f);
                        state->render_target->DrawTextW(status_text.c_str(), static_cast<UINT32>(status_text.size()), state->small_format.Get(), text_rect, state->text_white_brush.Get());
                    } else if (needs_download) {
                        bool dl_hovered = (state->mouse_pos.x >= 440 && state->mouse_pos.x <= 560 && state->mouse_pos.y >= 515 && state->mouse_pos.y <= 545);
                        draw_button(state, 440, 515, 560, 545, L"下载依赖", dl_hovered);

                        D2D1_RECT_F text_rect = D2D1::RectF(575.0f, 520.0f, 730.0f, 545.0f);
                        state->render_target->DrawTextW(status_text.c_str(), static_cast<UINT32>(status_text.size()), state->small_format.Get(), text_rect, state->text_grey_brush.Get());
                    } else {
                        state->render_target->DrawTextW(status_text.c_str(), static_cast<UINT32>(status_text.size()), state->small_format.Get(), status_rect, state->text_grey_brush.Get());
                    }

                    // Note text (moved down)
                    D2D1_RECT_F note_rect = D2D1::RectF(230.0f, 570.0f, 700.0f, 615.0f);
                    state->render_target->DrawTextW(strings::settings_note.data(), static_cast<UINT32>(strings::settings_note.size()), state->small_format.Get(), note_rect, state->text_grey_brush.Get());

                } else if (state->active_tab == 1) {
                    // Toolbar tools list reordering and visibility
                    std::vector<std::wstring> current_tools = split_by_comma(state->config.toolbar_order);

                    // Draw List Box Border & Background
                    D2D1_RECT_F list_rect = D2D1::RectF(230.0f, 80.0f, 480.0f, 580.0f);
                    state->render_target->FillRectangle(list_rect, state->control_bg_brush.Get());
                    state->render_target->DrawRectangle(list_rect, state->border_brush.Get(), 1.0f);

                    for (size_t i = 0; i < current_tools.size(); ++i) {
                        float y = 85.0f + i * 23.0f;
                        D2D1_RECT_F item_rect = D2D1::RectF(232.0f, y, 478.0f, y + 22.0f);
                        bool is_selected = (state->selected_tool_idx == static_cast<int>(i));
                        bool is_hovered = (state->mouse_pos.x >= 230 && state->mouse_pos.x <= 480 && state->mouse_pos.y >= y && state->mouse_pos.y <= y + 22.0f);
                        bool is_shown = !annotation_tool_hidden(state->config.annotation_hidden_tools, current_tools[i]);

                        if (is_selected) {
                            state->render_target->FillRectangle(item_rect, state->active_tab_brush.Get());
                        } else if (is_hovered) {
                            state->render_target->FillRectangle(item_rect, state->border_brush.Get());
                        }

                        std::wstring displayName = (is_shown ? L"● " : L"○ ") + get_tool_display_name(current_tools[i]);
                        ID2D1SolidColorBrush* text_brush = is_selected ? state->blue_brush.Get() : (is_shown ? state->text_white_brush.Get() : state->text_grey_brush.Get());
                        state->render_target->DrawTextW(displayName.c_str(), static_cast<UINT32>(displayName.size()), state->small_format.Get(), item_rect, text_brush);
                    }

                    // Draw Configuration Controls in the right side
                    int sel = state->selected_tool_idx;
                    if (sel >= 0 && sel < static_cast<int>(current_tools.size())) {
                        D2D1_RECT_F sub_header = D2D1::RectF(510.0f, 80.0f, 700.0f, 110.0f);
                        std::wstring sub_title = L"所选: " + get_tool_display_name(current_tools[sel]);
                        state->render_target->DrawTextW(sub_title.c_str(), static_cast<UINT32>(sub_title.size()), state->text_format.Get(), sub_header, state->text_white_brush.Get());

                        D2D1_RECT_F toggle_label = D2D1::RectF(510.0f, 130.0f, 640.0f, 160.0f);
                        state->render_target->DrawTextW(L"在工具栏中显示", static_cast<UINT32>(wcslen(L"在工具栏中显示")), state->small_format.Get(), toggle_label, state->text_white_brush.Get());

                        bool is_shown = !annotation_tool_hidden(state->config.annotation_hidden_tools, current_tools[sel]);
                        draw_switch(state, 650, 134, is_shown);

                        bool up_hovered = (state->mouse_pos.x >= 510 && state->mouse_pos.x <= 710 && state->mouse_pos.y >= 200 && state->mouse_pos.y <= 235);
                        draw_button(state, 510, 200, 710, 235, L"上移工具", up_hovered);

                        bool down_hovered = (state->mouse_pos.x >= 510 && state->mouse_pos.x <= 710 && state->mouse_pos.y >= 250 && state->mouse_pos.y <= 285);
                        draw_button(state, 510, 250, 710, 285, L"下移工具", down_hovered);
                    } else {
                        D2D1_RECT_F empty_rect = D2D1::RectF(510.0f, 120.0f, 710.0f, 300.0f);
                        state->render_target->DrawTextW(L"请在左侧列表中选择一个工具以调整显示顺序和启用状态。", static_cast<UINT32>(wcslen(L"请在左侧列表中选择一个工具以调整显示顺序和启用状态。")), state->small_format.Get(), empty_rect, state->text_grey_brush.Get());
                    }

                } else if (state->active_tab == 2) {
                    // Shortcuts two columns
                    const wchar_t* col1_labels[] = {
                        L"全局截图热键", L"全局 OCR 热键", L"屏幕识字快捷键", L"选择工具快捷键",
                        L"矩形工具快捷键", L"椭圆工具快捷键", L"直线工具快捷键", L"箭头工具快捷键"
                    };
                    const wchar_t* col2_labels[] = {
                        L"画笔工具快捷键", L"马赛克快捷键", L"模糊工具快捷键", L"高亮工具快捷键",
                        L"文本工具快捷键", L"序号工具快捷键", L"橡皮擦快捷键"
                    };

                    // Col 1
                    for (int i = 0; i < 8; ++i) {
                        float y = 80.0f + i * 50.0f;
                        D2D1_RECT_F text_rect = D2D1::RectF(220.0f, y + 10.0f, 335.0f, y + 36.0f);
                        state->render_target->DrawTextW(col1_labels[i], static_cast<UINT32>(wcslen(col1_labels[i])), state->text_format.Get(), text_rect, state->text_white_brush.Get());
                        
                        std::wstring* sh = get_shortcut_ptr(state->config, i);
                        bool is_capturing = (state->capturing_idx_ == i);
                        bool is_hovered = (state->mouse_pos.x >= 340 && state->mouse_pos.x <= 450 && state->mouse_pos.y >= y + 10 && state->mouse_pos.y <= y + 36);
                        draw_hotkey_box(state, 340, static_cast<int>(y) + 10, 450, static_cast<int>(y) + 36, sh ? sh->c_str() : L"", is_capturing, is_hovered);
                    }

                    // Col 2
                    for (int i = 8; i < 15; ++i) {
                        float y = 80.0f + (i - 8) * 50.0f;
                        D2D1_RECT_F text_rect = D2D1::RectF(480.0f, y + 10.0f, 595.0f, y + 36.0f);
                        state->render_target->DrawTextW(col2_labels[i - 8], static_cast<UINT32>(wcslen(col2_labels[i - 8])), state->text_format.Get(), text_rect, state->text_white_brush.Get());

                        std::wstring* sh = get_shortcut_ptr(state->config, i);
                        bool is_capturing = (state->capturing_idx_ == i);
                        bool is_hovered = (state->mouse_pos.x >= 600 && state->mouse_pos.x <= 710 && state->mouse_pos.y >= y + 10 && state->mouse_pos.y <= y + 36);
                        draw_hotkey_box(state, 600, static_cast<int>(y) + 10, 710, static_cast<int>(y) + 36, sh ? sh->c_str() : L"", is_capturing, is_hovered);
                    }
                }

                // 4. Draw Footer (Dividing Line & Save/Cancel buttons)
                state->render_target->DrawLine(D2D1::Point2F(0.0f, 620.0f), D2D1::Point2F(740.0f, 620.0f), state->border_brush.Get(), 1.0f);

                bool save_hovered = (state->mouse_pos.x >= 500 && state->mouse_pos.x <= 600 && state->mouse_pos.y >= 640 && state->mouse_pos.y <= 676);
                draw_button(state, 500, 640, 600, 676, L"保存", save_hovered);

                bool cancel_hovered = (state->mouse_pos.x >= 615 && state->mouse_pos.x <= 715 && state->mouse_pos.y >= 640 && state->mouse_pos.y <= 676);
                draw_button(state, 615, 640, 715, 676, L"取消", cancel_hovered);

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
            
            // Check sidebar tabs
            for (int i = 0; i < 3; ++i) {
                float ty = 60.0f + i * 60.0f;
                if (pt.x >= 10 && pt.x <= 190 && pt.y >= ty && pt.y <= ty + 44.0f) {
                    state->active_tab = i;
                    state->capturing_idx_ = -1;
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
            }

            // Check footer buttons
            if (pt.y >= 640 && pt.y <= 676) {
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
                // Switches y ranges: 80 + i * 55
                for (int i = 0; i < 6; ++i) {
                    int sy = 80 + i * 55 + 10;
                    if (pt.x >= 650 && pt.x <= 694 && pt.y >= sy && pt.y <= sy + 22) {
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
                // Font Family button
                if (pt.x >= 230 && pt.x <= 420 && pt.y >= 415 && pt.y <= 445) {
                    if (state->config.text_font_family == L"Microsoft YaHei") {
                        state->config.text_font_family = L"Consolas";
                    } else if (state->config.text_font_family == L"Consolas") {
                        state->config.text_font_family = L"SimSun";
                    } else if (state->config.text_font_family == L"SimSun") {
                        state->config.text_font_family = L"Arial";
                    } else {
                        state->config.text_font_family = L"Microsoft YaHei";
                    }
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
                // Reset serial button
                if (pt.x >= 550 && pt.x <= 694 && pt.y >= 415 && pt.y <= 445) {
                    state->config.annotation_next_serial = 1;
                    MessageBoxW(window, L"标注序号计数器已成功重置为 1。", L"设置", MB_OK | MB_ICONINFORMATION);
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
                // Bold switch
                if (pt.x >= 350 && pt.x <= 394 && pt.y >= 465 && pt.y <= 487) {
                    state->config.text_font_bold = !state->config.text_font_bold;
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
                // Italic switch
                if (pt.x >= 600 && pt.x <= 644 && pt.y >= 465 && pt.y <= 487) {
                    state->config.text_font_italic = !state->config.text_font_italic;
                    InvalidateRect(window, nullptr, TRUE);
                    return 0;
                }
                // OCR Engine selection button
                if (pt.x >= 230 && pt.x <= 420 && pt.y >= 515 && pt.y <= 545) {
                    bool is_downloading = false;
                    {
                        std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                        is_downloading = g_ocr_download.is_downloading;
                    }
                    if (!is_downloading) {
                        state->config.ocr_engine = (state->config.ocr_engine + 1) % 3;
                        
                        // Automatically trigger download if selected engine lacks dependencies
                        bool needs_download = false;
                        if (state->config.ocr_engine == 1) {
                            std::wstring wechat_dir = get_wechat_install_path();
                            std::wstring ocr_dir = find_wechat_ocr_exe_dir();
                            bool wechat_installed = !wechat_dir.empty() && !ocr_dir.empty();
                            bool dll_exists = check_dependency_exists(L"wechat_ocr_api.dll");
                            if (wechat_installed && !dll_exists) {
                                needs_download = true;
                            }
                        } else if (state->config.ocr_engine == 2) {
                            bool files_exist = check_dependency_exists(L"rapidocr_api.dll") &&
                                               check_dependency_exists(L"onnxruntime.dll") &&
                                               check_dependency_exists(L"models/ch_PP-OCRv4_det_infer.onnx");
                            if (!files_exist) {
                                needs_download = true;
                            }
                        }
                        
                        if (needs_download) {
                            start_ocr_download(window, state->config.ocr_download_url);
                        }
                        
                        InvalidateRect(window, nullptr, TRUE);
                    }
                    return 0;
                }
                // Download button
                if (pt.x >= 440 && pt.x <= 560 && pt.y >= 515 && pt.y <= 545) {
                    bool needs_download = false;
                    bool is_downloading = false;
                    {
                        std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                        is_downloading = g_ocr_download.is_downloading;
                    }
                    if (state->config.ocr_engine == 1) {
                        std::wstring wechat_dir = get_wechat_install_path();
                        std::wstring ocr_dir = find_wechat_ocr_exe_dir();
                        bool wechat_installed = !wechat_dir.empty() && !ocr_dir.empty();
                        bool dll_exists = check_dependency_exists(L"wechat_ocr_api.dll");
                        if (wechat_installed && !dll_exists) {
                            needs_download = true;
                        }
                    } else if (state->config.ocr_engine == 2) {
                        bool files_exist = check_dependency_exists(L"rapidocr_api.dll") &&
                                           check_dependency_exists(L"onnxruntime.dll") &&
                                           check_dependency_exists(L"models/ch_PP-OCRv4_det_infer.onnx");
                        if (!files_exist) {
                            needs_download = true;
                        }
                    }
                    if (needs_download && !is_downloading) {
                        start_ocr_download(window, state->config.ocr_download_url);
                    }
                    return 0;
                }
            } else if (state->active_tab == 1) {
                std::vector<std::wstring> current_tools = split_by_comma(state->config.toolbar_order);

                // 1. Check list box item click
                if (pt.x >= 230 && pt.x <= 480 && pt.y >= 80 && pt.y <= 580) {
                    int clicked_idx = static_cast<int>((pt.y - 85) / 23);
                    if (clicked_idx >= 0 && clicked_idx < static_cast<int>(current_tools.size())) {
                        state->selected_tool_idx = clicked_idx;
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                }

                // 2. Check configuration controls on the right side
                int sel = state->selected_tool_idx;
                if (sel >= 0 && sel < static_cast<int>(current_tools.size())) {
                    // Check toggle switch: x: 650..694, y: 134..156
                    if (pt.x >= 650 && pt.x <= 694 && pt.y >= 134 && pt.y <= 156) {
                        toggle_hidden_tool(state->config.annotation_hidden_tools, current_tools[sel]);
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                    // Check "上移工具" button: x: 510..710, y: 200..235
                    if (pt.x >= 510 && pt.x <= 710 && pt.y >= 200 && pt.y <= 235) {
                        if (sel > 0) {
                            std::swap(current_tools[sel], current_tools[sel - 1]);
                            state->config.toolbar_order = join_by_comma(current_tools);
                            state->selected_tool_idx = sel - 1;
                            InvalidateRect(window, nullptr, TRUE);
                        }
                        return 0;
                    }
                    // Check "下移工具" button: x: 510..710, y: 250..285
                    if (pt.x >= 510 && pt.x <= 710 && pt.y >= 250 && pt.y <= 285) {
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
                    int sy = 80 + i * 50 + 10;
                    if (pt.x >= 340 && pt.x <= 450 && pt.y >= sy && pt.y <= sy + 26) {
                        state->capturing_idx_ = i;
                        InvalidateRect(window, nullptr, TRUE);
                        return 0;
                    }
                }
                // Col 2: i = 8..14
                for (int i = 8; i < 15; ++i) {
                    int sy = 80 + (i - 8) * 50 + 10;
                    if (pt.x >= 600 && pt.x <= 710 && pt.y >= sy && pt.y <= sy + 26) {
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
            // Check tabs
            for (int i = 0; i < 3; ++i) {
                float ty = 60.0f + i * 60.0f;
                if (pt.x >= 10 && pt.x <= 190 && pt.y >= ty && pt.y <= ty + 44.0f) {
                    is_hovering_interactive = true;
                }
            }
            // Check footer buttons
            if (pt.y >= 640 && pt.y <= 676 && ((pt.x >= 500 && pt.x <= 600) || (pt.x >= 615 && pt.x <= 715))) {
                is_hovering_interactive = true;
            }
            // Check content area elements
            if (state->active_tab == 0) {
                for (int i = 0; i < 6; ++i) {
                    int sy = 80 + i * 55 + 10;
                    if (pt.x >= 650 && pt.x <= 694 && pt.y >= sy && pt.y <= sy + 22) {
                        is_hovering_interactive = true;
                    }
                }
                // Font Family button
                if (pt.x >= 230 && pt.x <= 420 && pt.y >= 415 && pt.y <= 445) {
                    is_hovering_interactive = true;
                }
                // Reset serial button
                if (pt.x >= 550 && pt.x <= 694 && pt.y >= 415 && pt.y <= 445) {
                    is_hovering_interactive = true;
                }
                // Bold switch
                if (pt.x >= 350 && pt.x <= 394 && pt.y >= 465 && pt.y <= 487) {
                    is_hovering_interactive = true;
                }
                // Italic switch
                if (pt.x >= 600 && pt.x <= 644 && pt.y >= 465 && pt.y <= 487) {
                    is_hovering_interactive = true;
                }
                // OCR Engine selection button
                if (pt.x >= 230 && pt.x <= 420 && pt.y >= 515 && pt.y <= 545) {
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
                if (pt.x >= 440 && pt.x <= 560 && pt.y >= 515 && pt.y <= 545) {
                    bool needs_download = false;
                    bool is_downloading = false;
                    {
                        std::lock_guard<std::mutex> lock(g_ocr_download.mutex);
                        is_downloading = g_ocr_download.is_downloading;
                    }
                    if (state->config.ocr_engine == 1) {
                        std::wstring wechat_dir = get_wechat_install_path();
                        std::wstring ocr_dir = find_wechat_ocr_exe_dir();
                        bool wechat_installed = !wechat_dir.empty() && !ocr_dir.empty();
                        bool dll_exists = check_dependency_exists(L"wechat_ocr_api.dll");
                        if (wechat_installed && !dll_exists) needs_download = true;
                    } else if (state->config.ocr_engine == 2) {
                        bool files_exist = check_dependency_exists(L"rapidocr_api.dll") &&
                                           check_dependency_exists(L"onnxruntime.dll") &&
                                           check_dependency_exists(L"models/ch_PP-OCRv4_det_infer.onnx");
                        if (!files_exist) needs_download = true;
                    }
                    if (needs_download && !is_downloading) {
                        is_hovering_interactive = true;
                    }
                }
            } else if (state->active_tab == 1) {
                std::vector<std::wstring> current_tools = split_by_comma(state->config.toolbar_order);
                // Hovering list items
                for (size_t i = 0; i < current_tools.size(); ++i) {
                    float y = 85.0f + i * 23.0f;
                    if (pt.x >= 230 && pt.x <= 480 && pt.y >= y && pt.y <= y + 22.0f) {
                        is_hovering_interactive = true;
                    }
                }
                // Hovering config controls
                int sel = state->selected_tool_idx;
                if (sel >= 0 && sel < static_cast<int>(current_tools.size())) {
                    if (pt.x >= 650 && pt.x <= 694 && pt.y >= 134 && pt.y <= 156) {
                        is_hovering_interactive = true;
                    }
                    if (pt.x >= 510 && pt.x <= 710 && pt.y >= 200 && pt.y <= 235) {
                        is_hovering_interactive = true;
                    }
                    if (pt.x >= 510 && pt.x <= 710 && pt.y >= 250 && pt.y <= 285) {
                        is_hovering_interactive = true;
                    }
                }
            } else if (state->active_tab == 2) {
                // Col 1 boxes
                for (int i = 0; i < 8; ++i) {
                    int sy = 80 + i * 50 + 10;
                    if (pt.x >= 340 && pt.x <= 450 && pt.y >= sy && pt.y <= sy + 26) {
                        is_hovering_interactive = true;
                    }
                }
                // Col 2 boxes
                for (int i = 8; i < 15; ++i) {
                    int sy = 80 + (i - 8) * 50 + 10;
                    if (pt.x >= 600 && pt.x <= 710 && pt.y >= sy && pt.y <= sy + 26) {
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
        window_class.lpfnWndProc = settings_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = CreateSolidBrush(RGB(30, 35, 43));
        window_class.lpszClassName = L"AirScreenshot.Settings";
        RegisterClassExW(&window_class);
    });

    SettingsState state;
    state.config = config;
    constexpr int window_width = 740;
    constexpr int window_height = 720;
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
    
    // Adjust window rect for caption and border to get exact client area
    RECT rect_win{ 0, 0, scaled_width, scaled_height };
    AdjustWindowRectExForDpi(&rect_win, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW, dpi);
    
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW,
                                  L"AirScreenshot.Settings",
                                  strings::settings_title.data(),
                                  WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  rect_win.right - rect_win.left,
                                  rect_win.bottom - rect_win.top,
                                  owner,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  &state);
    if (!window) {
        return false;
    }
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
