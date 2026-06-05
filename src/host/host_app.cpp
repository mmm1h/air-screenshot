#include "host_app.h"

#include "about_window.h"
#include "resource.h"
#include "settings_window.h"

#include "airshot/capture.h"
#include "airshot/ocr.h"
#include "airshot/output.h"
#include "airshot/overlay.h"
#include "airshot/portable.h"
#include "airshot/strings.h"

#include <shellapi.h>

#include <malloc.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <thread>

namespace airshot {
namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kDispatchRequest = WM_APP + 2;
constexpr UINT kShowSettings = WM_APP + 3;
constexpr UINT kTransientDone = WM_APP + 4;
constexpr int kHotkeyCapture = 1;
constexpr int kHotkeyOcr = 2;
constexpr UINT kMenuCapture = 2001;
constexpr UINT kMenuSettings = 2002;
constexpr UINT kMenuUpdate = 2003;
constexpr UINT kMenuExit = 2004;
constexpr UINT kMenuCloseAllPins = 2005;
constexpr UINT kMenuAbout = 2006;

RegionResult run_trimmed_region_capture(const RegionRequest& request) {
    RegionResult result = run_region_capture(request);
    _heapmin();
    HEAP_OPTIMIZE_RESOURCES_INFORMATION heap_information{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
    HeapSetInformation(nullptr, HeapOptimizeResources, &heap_information, sizeof(heap_information));
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    return result;
}

}  // namespace

HostApp::HostApp(HINSTANCE instance, bool transient) : instance_(instance), transient_(transient) {}

HostApp::~HostApp() {
    shutdown();
}

int HostApp::run() {
    if (!initialize()) {
        return static_cast<int>(ExitCode::ipc_failed);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    shutdown();
    return static_cast<int>(message.wParam);
}

bool HostApp::initialize() {
    mutex_ = CreateMutexW(nullptr, TRUE, kHostMutexName);
    if (!mutex_ || GetLastError() == ERROR_ALREADY_EXISTS) {
        return false;
    }

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    window_class.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    window_class.lpszClassName = kAppWindowClass;
    RegisterClassExW(&window_class);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW,
                              kAppWindowClass,
                              kAppName,
                              WS_OVERLAPPED,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              1,
                              1,
                              nullptr,
                              nullptr,
                              instance_,
                              this);
    if (!window_) {
        return false;
    }

    config_ = load_config();
    pipe_server_.start([this](std::wstring_view request) {
        RequestContext context{std::wstring(request), {}};
        SendMessageW(window_, kDispatchRequest, 0, reinterpret_cast<LPARAM>(&context));
        if (transient_) {
            PostMessageW(window_, kTransientDone, 0, 0);
        }
        return context.response;
    });
    if (!transient_) {
        apply_shell();
        sync_startup_task();
        check_for_updates(false);
    }
    return true;
}

void HostApp::shutdown() {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    unregister_hotkeys();
    remove_tray();
    pipe_server_.stop();
    pin_windows_.clear();
    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (mutex_) {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}

void HostApp::apply_shell() {
    unregister_hotkeys();
    remove_tray();
    if (config_.shell_enabled) {
        features_.activate(L"shell", config_);
        add_tray();
        register_hotkeys();
    }
}

void HostApp::sync_startup_task() {
    sync_portable_startup(config_.shell_enabled && config_.start_at_login);
}

void HostApp::add_tray() {
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = window_;
    tray_.uID = 1;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = kTrayMessage;
    tray_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wcscpy_s(tray_.szTip, kAppName);
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &tray_) == TRUE;
    if (tray_added_) {
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }
}

void HostApp::remove_tray() {
    if (tray_added_) {
        Shell_NotifyIconW(NIM_DELETE, &tray_);
        tray_added_ = false;
    }
}

void HostApp::register_hotkeys() {
    if (const auto capture = parse_hotkey(config_.capture_hotkey)) {
        if (!RegisterHotKey(window_, kHotkeyCapture, capture->modifiers, capture->virtual_key)) {
            notify(kAppName, strings::hotkey_conflict);
        }
    }
    if (config_.ocr_enabled && config_.global_ocr_enabled) {
        if (const auto ocr = parse_hotkey(config_.global_ocr_hotkey)) {
            if (!RegisterHotKey(window_, kHotkeyOcr, ocr->modifiers, ocr->virtual_key)) {
                notify(kAppName, strings::hotkey_conflict);
            }
        }
    }
}

void HostApp::unregister_hotkeys() {
    if (window_) {
        UnregisterHotKey(window_, kHotkeyCapture);
        UnregisterHotKey(window_, kHotkeyOcr);
    }
}

void HostApp::show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuCapture, strings::tray_capture.data());
    AppendMenuW(menu, MF_STRING, kMenuSettings, strings::tray_settings.data());
    AppendMenuW(menu, MF_STRING, kMenuUpdate, strings::tray_update.data());
    if (!pin_windows_.empty()) {
        AppendMenuW(menu, MF_STRING, kMenuCloseAllPins, L"销毁所有贴图 (Close All Pins)");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, strings::tray_exit.data());
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window_);
    const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (selected != 0) {
        PostMessageW(window_, WM_COMMAND, selected, 0);
    }
}

void HostApp::show_settings() {
    AppConfig edited = config_;
    if (show_settings_window(window_, edited)) {
        config_ = std::move(edited);
        std::wstring error;
        if (!save_config(config_, &error)) {
            MessageBoxW(window_, error.c_str(), kAppName, MB_OK | MB_ICONERROR);
            return;
        }
        features_.unload_disabled(config_);
        apply_shell();
        sync_startup_task();
        if (!config_.shell_enabled && !transient_) {
            PostMessageW(window_, WM_CLOSE, 0, 0);
        }
    }
}

void HostApp::notify(std::wstring_view title, std::wstring_view message) {
    if (!tray_added_) {
        return;
    }
    tray_.uFlags = NIF_INFO;
    wcsncpy_s(tray_.szInfoTitle, title.data(), _TRUNCATE);
    wcsncpy_s(tray_.szInfo, message.data(), _TRUNCATE);
    tray_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

void HostApp::capture_region(RegionAction action) {
    if (action == RegionAction::interactive && config_.annotation_enabled) {
        features_.activate(L"annotation", config_);
    } else if (action == RegionAction::ocr) {
        features_.activate(L"ocr", config_);
    }
    RegionRequest request;
    request.action = action;
    request.config = config_;
    request.copy_ocr = true;
    const auto result = run_trimmed_region_capture(request);
    if (result.config.custom_color != config_.custom_color) {
        config_.custom_color = result.config.custom_color;
        save_config(config_);
    }
    if (result.code == ExitCode::success) {
        if (result.action == RegionAction::pin) {
            auto pin = PinWindow::create(instance_, window_, result.bitmap, result.bounds.left, result.bounds.top);
            if (pin) {
                pin_windows_.push_back(std::move(pin));
            }
        } else {
            if (config_.notifications_enabled) {
                notify(kAppName, result.message);
            }
        }
    } else if (result.code != ExitCode::user_cancelled) {
        notify(kAppName, result.message);
    }
}

CommandResponse HostApp::execute_request(std::wstring_view request_json) {
    const ScopedWinrtApartment apartment(true);
    if (!apartment.available()) {
        return {ExitCode::unknown_error, L"无法初始化 Windows Runtime。"};
    }
    try {
        const JsonObject request = JsonObject::Parse(request_json);
        const std::wstring command = request.GetNamedString(L"command", L"").c_str();
        if (command == L"capture") {
            return execute_capture(request);
        }
        if (command == L"ocr") {
            return execute_ocr(request);
        }
        if (command == L"module") {
            return execute_module(request);
        }
        if (command == L"app") {
            return execute_app(request);
        }
        return {ExitCode::invalid_arguments, L"未知宿主命令。"};
    } catch (const winrt::hresult_error& error) {
        return {ExitCode::invalid_arguments, std::format(L"请求无效：{}", error.message().c_str())};
    }
}

CommandResponse HostApp::execute_capture(const JsonObject& request) {
    const std::wstring mode = request.GetNamedString(L"mode", L"").c_str();
    const std::wstring output = request.GetNamedString(L"output", L"").c_str();
    const std::wstring path = request.GetNamedString(L"path", L"").c_str();
    if (mode == L"region") {
        RegionRequest region;
        region.config = config_;
        region.path = path;
        if (output == L"clipboard") {
            region.action = RegionAction::clipboard;
        } else if (output == L"file") {
            region.action = RegionAction::file;
        } else {
            region.action = RegionAction::interactive;
            features_.activate(L"annotation", config_);
        }
        const auto result = run_trimmed_region_capture(region);
        if (result.config.custom_color != config_.custom_color) {
            config_.custom_color = result.config.custom_color;
            save_config(config_);
        }
        return {result.code, result.message, result.path, result.text};
    }
    if (mode == L"window") {
        const auto captured = capture_active_window();
        if (!captured) {
            return {ExitCode::operation_failed, std::wstring(strings::capture_failed)};
        }
        return output_bitmap(captured->first, output, path, L"活动窗口截图完成。");
    }
    if (mode == L"screen") {
        const std::wstring monitor = request.GetNamedString(L"monitor", L"all").c_str();
        const auto captured = capture_monitor(monitor);
        if (!captured) {
            return {ExitCode::operation_failed, L"显示器不存在或截图失败。"};
        }
        return output_bitmap(captured->first, output, path, L"屏幕截图完成。");
    }
    return {ExitCode::invalid_arguments, L"未知截图模式。"};
}

CommandResponse HostApp::execute_ocr(const JsonObject& request) {
    if (!config_.ocr_enabled) {
        return {ExitCode::module_unavailable, L"OCR 模块已关闭。"};
    }
    features_.activate(L"ocr", config_);
    RegionRequest region;
    region.config = config_;
    region.action = RegionAction::ocr;
    region.copy_ocr = request.GetNamedBoolean(L"copy", false);
    const auto result = run_trimmed_region_capture(region);
    if (result.config.custom_color != config_.custom_color) {
        config_.custom_color = result.config.custom_color;
        save_config(config_);
    }
    return {result.code, result.message, result.path, result.text};
}

CommandResponse HostApp::execute_module(const JsonObject& request) {
    const std::wstring action = request.GetNamedString(L"action", L"").c_str();
    if (action == L"list") {
        JsonObject data;
        for (const auto& [name, enabled] : features_.list(config_)) {
            data.SetNamedValue(name, JsonValue::CreateBooleanValue(enabled));
        }
        return {ExitCode::success, L"模块状态读取完成。", {}, {}, std::wstring(data.Stringify().c_str())};
    }
    const std::wstring module = request.GetNamedString(L"module", L"").c_str();
    const bool enabled = action == L"enable";
    if (module == L"annotation") {
        config_.annotation_enabled = enabled;
    } else if (module == L"ocr") {
        config_.ocr_enabled = enabled;
    } else if (module == L"shell") {
        config_.shell_enabled = enabled;
    } else {
        return {ExitCode::invalid_arguments, L"未知模块。"};
    }
    std::wstring error;
    if (!save_config(config_, &error)) {
        return {ExitCode::operation_failed, error};
    }
    features_.unload_disabled(config_);
    if (!transient_) {
        apply_shell();
        sync_startup_task();
        if (!config_.shell_enabled) {
            PostMessageW(window_, WM_CLOSE, 0, 0);
        }
    } else {
        sync_startup_task();
    }
    return {ExitCode::success, enabled ? L"模块已启用。" : L"模块已关闭。"};
}

CommandResponse HostApp::execute_app(const JsonObject& request) {
    const std::wstring action = request.GetNamedString(L"action", L"").c_str();
    if (action == L"status" || action == L"start") {
        JsonObject data;
        data.SetNamedValue(L"running", JsonValue::CreateBooleanValue(true));
        data.SetNamedValue(L"transient", JsonValue::CreateBooleanValue(transient_));
        data.SetNamedValue(L"shell", JsonValue::CreateBooleanValue(config_.shell_enabled));
        return {ExitCode::success, L"Air Screenshot 正在运行。", {}, {}, std::wstring(data.Stringify().c_str())};
    }
    if (action == L"stop") {
        PostMessageW(window_, WM_CLOSE, 0, 0);
        return {ExitCode::success, L"Air Screenshot 已退出。"};
    }
    if (action == L"settings") {
        PostMessageW(window_, kShowSettings, 0, 0);
        return {ExitCode::success, L"已打开设置。"};
    }
    return {ExitCode::invalid_arguments, L"未知 app 操作。"};
}

CommandResponse HostApp::output_bitmap(
    Bitmap bitmap, std::wstring_view output, std::wstring_view requested_path, std::wstring_view success_message) {
    if (bitmap.empty()) {
        return {ExitCode::operation_failed, std::wstring(strings::capture_failed)};
    }
    std::wstring error;
    if (output == L"file") {
        const auto path = resolve_output_path(requested_path);
        if (!save_png(bitmap, path, &error)) {
            return {ExitCode::operation_failed, error};
        }
        return {ExitCode::success, std::wstring(success_message), path.wstring()};
    }
    if (!copy_bitmap_to_clipboard(bitmap, &error)) {
        return {ExitCode::operation_failed, error};
    }
    return {ExitCode::success, std::wstring(success_message)};
}

LRESULT HostApp::handle_message(UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_PIN_WINDOW_CLOSED) {
        auto* pin_ptr = reinterpret_cast<PinWindow*>(l_param);
        std::erase_if(pin_windows_, [pin_ptr](const auto& pin) {
            return pin.get() == pin_ptr;
        });
        return 0;
    }
    if (message == kDispatchRequest) {
        auto* context = reinterpret_cast<RequestContext*>(l_param);
        context->response = response_to_json(execute_request(context->request));
        return 0;
    }
    if (message == kShowSettings) {
        show_settings();
        return 0;
    }
    if (message == kTransientDone) {
        PostMessageW(window_, WM_CLOSE, 0, 0);
        return 0;
    }
    if (message == kTrayMessage) {
        const UINT event = LOWORD(l_param);
        if (event == WM_LBUTTONUP || event == NIN_SELECT) {
            capture_region(RegionAction::interactive);
        } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            show_tray_menu();
        }
        return 0;
    }
    if (message == WM_HOTKEY) {
        if (w_param == kHotkeyCapture) {
            capture_region(RegionAction::interactive);
        } else if (w_param == kHotkeyOcr) {
            capture_region(RegionAction::ocr);
        }
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(w_param) == kMenuCapture) {
            capture_region(RegionAction::interactive);
        } else if (LOWORD(w_param) == kMenuSettings) {
            show_settings();
        } else if (LOWORD(w_param) == kMenuUpdate) {
            check_for_updates(true);
        } else if (LOWORD(w_param) == kMenuCloseAllPins) {
            pin_windows_.clear();
        } else if (LOWORD(w_param) == kMenuExit) {
            PostMessageW(window_, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        remove_tray();
        unregister_hotkeys();
        DestroyWindow(window_);
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, w_param, l_param);
}

LRESULT CALLBACK HostApp::window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* app = reinterpret_cast<HostApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        app = static_cast<HostApp*>(create->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handle_message(message, w_param, l_param) : DefWindowProcW(window, message, w_param, l_param);
}

void HostApp::check_for_updates(bool user_triggered) {
    const HWND owner_window = window_;
    std::thread([owner_window, user_triggered]() {
        const ScopedWinrtApartment apartment;
        std::wstring message;
        const UpdateStageResult result = apartment.available() ? stage_latest_update(&message) : UpdateStageResult::failed;
        if (!apartment.available()) {
            message = L"无法初始化更新检查所需的 Windows 运行时。";
        }
        const bool writable_error =
            message.find(L"当前目录不可写") != std::wstring::npos || message.find(L"只读") != std::wstring::npos;
        if (user_triggered || writable_error) {
            const UINT icon = result == UpdateStageResult::failed ? MB_ICONERROR : MB_ICONINFORMATION;
            MessageBoxW(owner_window, message.c_str(), kAppName, MB_OK | icon);
        }
    }).detach();
}

}  // namespace airshot
