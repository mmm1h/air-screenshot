#include "host_app.h"

#include "about_window.h"
#include "resource.h"
#include "settings_window.h"

#include "airshot/capture.h"
#include "airshot/output.h"
#include "airshot/strings.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <chrono>
#include <exception>
#include <new>
#include <utility>

namespace airshot {
namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kDispatchRequest = WM_APP + 2;
constexpr UINT kCaptureCompleted = WM_APP + 3;
constexpr UINT kRequestTimedOut = WM_APP + 4;
constexpr UINT kResponseCompleted = WM_APP + 5;
constexpr UINT kUpdateCompleted = WM_APP + 6;
constexpr UINT kTransportCompleted = WM_APP + 7;
constexpr UINT kTransientResponseDelivered = WM_APP + 8;
constexpr UINT kTransientDeliveryFailed = WM_APP + 9;
constexpr UINT kIpcListenerFaulted = WM_APP + 11;

constexpr int kHotkeyCapture = 1;
constexpr int kHotkeyOcr = 2;

constexpr UINT kMenuCapture = 2001;
constexpr UINT kMenuSettings = 2002;
constexpr UINT kMenuUpdate = 2003;
constexpr UINT kMenuExit = 2004;
constexpr UINT kMenuCloseAllPins = 2005;
constexpr UINT kMenuAbout = 2006;
constexpr UINT_PTR kShutdownDrainTimer = 1;
constexpr UINT_PTR kTransientIdleTimer = 2;

constexpr auto kRequestTimeout = std::chrono::seconds(90);
constexpr auto kRequestCancellationPoll = std::chrono::milliseconds(50);
constexpr DWORD kTransientStartupTimeoutMs = 20000;
constexpr DWORD kTransientSuccessGraceMs = 500;
constexpr DWORD kTransientFailureTimeoutMs = 2000;

std::wstring monitor_selector(const MonitorTarget& monitor) {
    switch (monitor.kind) {
        case MonitorTargetKind::all: return L"all";
        case MonitorTargetKind::primary: return L"primary";
        case MonitorTargetKind::cursor: return L"cursor";
        case MonitorTargetKind::index: return std::to_wstring(monitor.index);
    }
    return L"all";
}

std::wstring module_status_json(const AppConfig& config) {
    JsonObject data;
    data.SetNamedValue(L"annotation", JsonValue::CreateBooleanValue(config.annotation_enabled));
    data.SetNamedValue(L"ocr", JsonValue::CreateBooleanValue(config.ocr_enabled));
    data.SetNamedValue(L"shell", JsonValue::CreateBooleanValue(config.shell_enabled));
    return data.Stringify().c_str();
}

std::wstring app_status_json(const AppConfig& config, bool transient) {
    JsonObject data;
    data.SetNamedValue(L"running", JsonValue::CreateBooleanValue(true));
    data.SetNamedValue(L"transient", JsonValue::CreateBooleanValue(transient));
    data.SetNamedValue(L"shell", JsonValue::CreateBooleanValue(config.shell_enabled));
    return data.Stringify().c_str();
}

CommandResponse shutting_down_response() {
    CommandResponse response;
    response.code = ExitCode::operation_failed;
    response.message = L"Air Screenshot 正在退出。";
    response.error_type = L"shutting_down";
    return response;
}

std::wstring serialize_response(const CommandResponse& response) {
    try {
        return response_to_json(response);
    } catch (...) {
        return LR"({"v":1,"ok":false,"code":1,"message":"无法序列化宿主响应。","error":{"type":"internal"}})";
    }
}

std::wstring launch_nonce_from_request(std::wstring_view request_text) {
    const ScopedWinrtApartment apartment;
    if (!apartment.available()) {
        return {};
    }
    try {
        const JsonObject request = JsonObject::Parse(request_text);
        if (!request.HasKey(L"launchNonce") ||
            request.GetNamedValue(L"launchNonce").ValueType() !=
                winrt::Windows::Data::Json::JsonValueType::String) {
            return {};
        }
        return request.GetNamedString(L"launchNonce").c_str();
    } catch (...) {
        return {};
    }
}

}  // namespace

HostApp::HostApp(
    HINSTANCE instance, bool transient, std::wstring transient_launch_nonce)
    : instance_(instance),
      transient_(transient),
      transient_launch_nonce_(std::move(transient_launch_nonce)) {}

HostApp::~HostApp() {
    shutdown();
}

int HostApp::run() {
    if (!initialize()) {
        return static_cast<int>(ExitCode::ipc_failed);
    }

    int exit_code = static_cast<int>(ExitCode::success);
    MSG message{};
    while (true) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        if (status < 0) {
            exit_code = static_cast<int>(ExitCode::unknown_error);
        } else {
            exit_code = static_cast<int>(message.wParam);
        }
        break;
    }

    shutdown();
    return exit_code;
}

bool HostApp::initialize() {
    mutex_ = CreateMutexW(nullptr, TRUE, kHostMutexName);
    if (!mutex_) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return false;
    }

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    window_class.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    window_class.lpszClassName = kAppWindowClass;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
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

    std::wstring config_error;
    auto loaded_config = config_store_.load(&config_error);
    if (!loaded_config) {
        if (config_error.empty()) {
            config_error = L"无法加载 Air Screenshot 配置。";
        }
        if (!transient_.load(std::memory_order_acquire)) {
            MessageBoxW(window_, config_error.c_str(), kAppName, MB_OK | MB_ICONERROR);
        }
        return false;
    }
    if (loaded_config->write_protected) {
        const std::wstring message = std::format(
            L"配置 schema {} 高于当前版本支持的 schema {}。为避免修改较新版本的配置或开机启动状态，"
            L"本版本不会启动。",
            loaded_config->schema_version,
            kCurrentConfigSchemaVersion);
        if (!transient_.load(std::memory_order_acquire)) {
            MessageBoxW(window_, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
        }
        return false;
    }
    config_ = std::move(*loaded_config);
    const HWND dispatch_window = window_;
    const DWORD dispatch_thread = GetCurrentThreadId();
    accepting_requests_.store(true, std::memory_order_release);
    if (!pipe_server_.start_correlated(
            [this, dispatch_window](
                std::wstring_view request, const PipeRequestContext& pipe_context) {
                responses_in_flight_.fetch_add(1, std::memory_order_acq_rel);
                try {
                    return handle_pipe_request(dispatch_window, request, pipe_context);
                } catch (...) {
                    return std::wstring(
                        LR"({"v":1,"ok":false,"code":1,"message":"宿主处理请求时发生异常。","error":{"type":"internal"}})");
                }
            },
            [this, dispatch_window](
                std::uint64_t correlation_id, std::wstring_view, bool sent) {
                note_response_sent(dispatch_window, correlation_id, sent);
            },
            [this, dispatch_window, dispatch_thread](DWORD) {
                accepting_requests_.store(false, std::memory_order_release);
                ipc_listener_faulted_.store(true, std::memory_order_release);
                if (!PostMessageW(dispatch_window, kIpcListenerFaulted, 0, 0)) {
                    PostThreadMessageW(
                        dispatch_thread,
                        WM_QUIT,
                        static_cast<WPARAM>(ExitCode::ipc_failed),
                        0);
                }
            })) {
        accepting_requests_.store(false, std::memory_order_release);
        return false;
    }

    if (transient_.load(std::memory_order_acquire)) {
        arm_transient_idle_shutdown(kTransientStartupTimeoutMs);
    } else {
        apply_shell();
        std::wstring startup_error;
        if (!sync_startup_task(config_, &startup_error) && !startup_error.empty()) {
            notify(kAppName, startup_error);
        }
        check_for_updates(false);
    }
    return true;
}

void HostApp::shutdown() {
    if (shutdown_complete_) {
        return;
    }
    shutdown_complete_ = true;
    accepting_requests_.store(false, std::memory_order_release);
    mode_ = UiMode::shutting_down;

    unregister_hotkeys();
    remove_tray();
    if (window_) {
        KillTimer(window_, kShutdownDrainTimer);
        KillTimer(window_, kTransientIdleTimer);
    }
    reject_pending_requests();

    if (capture_session_) {
        capture_session_->cancel();
        capture_session_.reset();
    }
    capture_completion_.reset();
    capture_request_.reset();

    if (settings_window_ && IsWindow(settings_window_)) {
        DestroyWindow(settings_window_);
    }
    settings_window_ = nullptr;
    if (about_window_ && IsWindow(about_window_)) {
        DestroyWindow(about_window_);
    }
    about_window_ = nullptr;

    for (const auto& pin : pin_windows_) {
        pin->request_close();
    }
    pipe_server_.stop();

    if (update_thread_.joinable()) {
        update_thread_.request_stop();
        update_thread_.join();
    }
    update_running_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(update_mutex_);
        update_completion_.reset();
    }

    if (window_ && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    window_ = nullptr;

    if (mutex_) {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}

void HostApp::request_shutdown() {
    if (mode_ == UiMode::shutting_down) {
        maybe_finish_shutdown();
        return;
    }
    mode_ = UiMode::shutting_down;
    accepting_requests_.store(false, std::memory_order_release);
    pipe_server_.stop_accepting();
    if (window_) {
        KillTimer(window_, kTransientIdleTimer);
        SetTimer(window_, kShutdownDrainTimer, 50, nullptr);
    }
    unregister_hotkeys();
    remove_tray();
    reject_pending_requests();

    if (update_thread_.joinable()) {
        update_thread_.request_stop();
    }
    if (capture_session_) {
        capture_session_->cancel();
    }
    if (settings_window_ && IsWindow(settings_window_)) {
        DestroyWindow(settings_window_);
    }
    if (about_window_ && IsWindow(about_window_)) {
        DestroyWindow(about_window_);
    }
    for (const auto& pin : pin_windows_) {
        pin->request_close();
    }
    prune_closed_pin_windows();
    maybe_finish_shutdown();
}

void HostApp::maybe_finish_shutdown() {
    prune_closed_pin_windows();
    if (shutdown_complete_ || mode_ != UiMode::shutting_down ||
        !pipe_server_.wait_until_not_accepting(0) || pipe_server_.active_clients() != 0 ||
        responses_in_flight_.load(std::memory_order_acquire) != 0 || capture_session_ || capture_completion_ ||
        (settings_window_ && IsWindow(settings_window_)) || (about_window_ && IsWindow(about_window_)) ||
        !pin_windows_.empty()) {
        return;
    }

    const HWND window = window_;
    if (window && IsWindow(window)) {
        KillTimer(window, kShutdownDrainTimer);
        DestroyWindow(window);
    }
    if (window_ == window) {
        window_ = nullptr;
    }
    const ExitCode exit_code =
        ipc_listener_faulted_.load(std::memory_order_acquire)
            ? ExitCode::ipc_failed
            : ExitCode::success;
    PostQuitMessage(static_cast<int>(exit_code));
}

void HostApp::reject_pending_requests() {
    std::vector<std::shared_ptr<RequestContext>> pending;
    {
        std::lock_guard lock(request_mutex_);
        dispatch_queue_.clear();
        pending.reserve(pending_requests_.size());
        for (const auto& [id, request] : pending_requests_) {
            (void)id;
            pending.push_back(request);
        }
    }

    for (const auto& request : pending) {
        request->cancelled.store(true, std::memory_order_release);
        complete_request(request, shutting_down_response());
    }
}

void HostApp::drain_dispatch_queue() {
    std::deque<std::shared_ptr<RequestContext>> queued;
    {
        std::lock_guard lock(request_mutex_);
        queued.swap(dispatch_queue_);
    }

    for (auto& request : queued) {
        if (!request || request->completed.load(std::memory_order_acquire) ||
            request->cancelled.load(std::memory_order_acquire)) {
            continue;
        }
        if (mode_ == UiMode::shutting_down) {
            complete_request(request, shutting_down_response());
            continue;
        }
        try {
            dispatch_request(request);
        } catch (...) {
            CommandResponse failure;
            failure.code = ExitCode::unknown_error;
            failure.message = L"宿主处理请求时发生异常。";
            failure.error_type = L"internal";
            complete_request(request, std::move(failure));
        }
    }
}

void HostApp::note_response_sent(HWND dispatch_window, std::uint64_t correlation_id, bool sent) {
    std::optional<ShutdownResponse> shutdown_response;
    {
        std::lock_guard lock(acknowledgement_mutex_);
        const auto found = shutdown_responses_.find(correlation_id);
        if (found != shutdown_responses_.end()) {
            shutdown_response = found->second;
            shutdown_responses_.erase(found);
        }
    }
    const bool transient_response_is_current =
        shutdown_response && shutdown_response->transient_generation != 0 &&
        transient_.load(std::memory_order_acquire) &&
        shutdown_response->transient_generation ==
            transient_generation_.load(std::memory_order_acquire);
    std::uint32_t in_flight = responses_in_flight_.load(std::memory_order_acquire);
    while (in_flight != 0 &&
           !responses_in_flight_.compare_exchange_weak(
               in_flight, in_flight - 1, std::memory_order_acq_rel)) {
    }
    if (shutdown_response && shutdown_response->explicit_request) {
        PostMessageW(dispatch_window, kResponseCompleted, 0, 0);
    } else if (transient_response_is_current && sent) {
        PostMessageW(dispatch_window, kTransientResponseDelivered, 0, 0);
        PostMessageW(dispatch_window, kTransportCompleted, 0, 0);
    } else {
        if (transient_response_is_current) {
            transient_primary_assigned_.store(false, std::memory_order_release);
            PostMessageW(dispatch_window, kTransientDeliveryFailed, 0, 0);
        }
        PostMessageW(dispatch_window, kTransportCompleted, 0, 0);
    }
}

void HostApp::remember_shutdown_response(const std::shared_ptr<RequestContext>& request) {
    if (!request ||
        (!request->explicit_shutdown_after_response.load(std::memory_order_acquire) &&
         request->transient_generation.load(std::memory_order_acquire) == 0)) {
        return;
    }
    const bool explicit_request =
        request->explicit_shutdown_after_response.load(std::memory_order_acquire);
    const std::uint64_t generation =
        request->transient_generation.load(std::memory_order_acquire);
    std::lock_guard lock(acknowledgement_mutex_);
    shutdown_responses_.insert_or_assign(
        request->id,
        ShutdownResponse{explicit_request, generation});
}

void HostApp::arm_transient_idle_shutdown(DWORD delay_ms) {
    if (!window_ || !transient_.load(std::memory_order_acquire) ||
        mode_ == UiMode::shutting_down) {
        return;
    }
    transient_idle_deadline_ = GetTickCount64() + delay_ms;
    SetTimer(window_, kTransientIdleTimer, 250, nullptr);
}

void HostApp::cancel_transient_idle_shutdown() {
    transient_idle_deadline_ = 0;
    if (window_) {
        KillTimer(window_, kTransientIdleTimer);
    }
}

void HostApp::maybe_shutdown_idle_transient() {
    if (!transient_.load(std::memory_order_acquire)) {
        cancel_transient_idle_shutdown();
        return;
    }
    if (transient_idle_deadline_ == 0 ||
        GetTickCount64() < transient_idle_deadline_ ||
        mode_ != UiMode::idle ||
        responses_in_flight_.load(std::memory_order_acquire) != 0 ||
        pipe_server_.active_clients() != 0 ||
        capture_session_ ||
        capture_completion_ ||
        (settings_window_ && IsWindow(settings_window_)) ||
        (about_window_ && IsWindow(about_window_))) {
        return;
    }

    bool has_pending_requests = false;
    {
        std::lock_guard lock(request_mutex_);
        has_pending_requests = !pending_requests_.empty() || !dispatch_queue_.empty();
    }
    if (!has_pending_requests) {
        request_shutdown();
    }
}

void HostApp::promote_to_persistent() {
    if (!transient_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    transient_primary_assigned_.store(false, std::memory_order_release);
    transient_generation_.fetch_add(1, std::memory_order_acq_rel);
    cancel_transient_idle_shutdown();
    apply_shell();
    std::wstring startup_error;
    if (!sync_startup_task(config_, &startup_error) && !startup_error.empty()) {
        notify(kAppName, startup_error);
    }
    check_for_updates(false);
}

void HostApp::apply_shell() {
    unregister_hotkeys();
    remove_tray();
    if (config_.shell_enabled) {
        add_tray();
        register_hotkeys();
    }
}

bool HostApp::sync_startup_task(const AppConfig& config, std::wstring* error) {
    return sync_portable_startup(config.shell_enabled && config.start_at_login, error);
}

bool HostApp::commit_config(AppConfig next, std::wstring* error) {
    if (error) {
        error->clear();
    }

    std::wstring startup_error;
    if (!sync_startup_task(next, &startup_error)) {
        if (error) {
            *error = startup_error.empty()
                ? L"无法同步开机启动设置。"
                : L"无法同步开机启动设置：" + startup_error;
        }
        return false;
    }

    std::wstring save_error;
    if (!config_store_.save(next, &save_error)) {
        std::wstring rollback_error;
        const bool startup_rolled_back = sync_startup_task(config_, &rollback_error);
        if (error) {
            *error = save_error.empty() ? L"无法保存 Air Screenshot 配置。" : std::move(save_error);
            if (!startup_rolled_back) {
                *error += rollback_error.empty()
                    ? L" 开机启动设置也无法回滚。"
                    : L" 开机启动设置无法回滚：" + rollback_error;
            }
        }
        return false;
    }

    config_ = std::move(next);
    return true;
}

void HostApp::prune_closed_pin_windows() {
    std::erase_if(pin_windows_, [](const auto& pin) {
        const HWND hwnd = pin ? pin->hwnd() : nullptr;
        return !hwnd || !IsWindow(hwnd);
    });
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
    if (!window_ || !config_.shell_enabled || mode_ != UiMode::idle) {
        return;
    }
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
    if (mode_ == UiMode::shutting_down) {
        return;
    }
    prune_closed_pin_windows();
    const HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kMenuCapture, strings::tray_capture.data());
    AppendMenuW(menu, MF_STRING, kMenuSettings, strings::tray_settings.data());
    AppendMenuW(menu, MF_STRING, kMenuUpdate, strings::tray_update.data());
    AppendMenuW(menu, MF_STRING, kMenuAbout, L"关于");
    if (!pin_windows_.empty()) {
        AppendMenuW(menu, MF_STRING, kMenuCloseAllPins, L"销毁所有贴图");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, strings::tray_exit.data());

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window_);
    const UINT selected =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (selected != 0) {
        PostMessageW(window_, WM_COMMAND, selected, 0);
    }
}

bool HostApp::show_settings() {
    if (mode_ != UiMode::idle) {
        if (settings_window_ && IsWindow(settings_window_)) {
            SetForegroundWindow(settings_window_);
        }
        return false;
    }

    mode_ = UiMode::settings;
    unregister_hotkeys();
    settings_window_ = show_settings_window_async(
        window_,
        config_,
        [this](std::optional<AppConfig> edited) {
            settings_window_ = nullptr;
            if (mode_ == UiMode::shutting_down) {
                maybe_finish_shutdown();
                return;
            }

            if (!edited) {
                mode_ = UiMode::idle;
                if (config_.shell_enabled) {
                    register_hotkeys();
                }
                return;
            }

            std::wstring error;
            if (!commit_config(std::move(*edited), &error)) {
                MessageBoxW(window_, error.c_str(), kAppName, MB_OK | MB_ICONERROR);
                if (mode_ == UiMode::shutting_down) {
                    maybe_finish_shutdown();
                    return;
                }
                mode_ = UiMode::idle;
                if (config_.shell_enabled) {
                    register_hotkeys();
                }
                return;
            }

            apply_shell();
            if (!config_.shell_enabled && !transient_.load(std::memory_order_acquire)) {
                request_shutdown();
                return;
            }
            if (mode_ != UiMode::shutting_down) {
                mode_ = UiMode::idle;
                if (config_.shell_enabled) {
                    register_hotkeys();
                }
            }
        });

    if (!settings_window_ && mode_ == UiMode::settings) {
        mode_ = UiMode::idle;
        if (config_.shell_enabled) {
            register_hotkeys();
        }
    }
    return settings_window_ != nullptr;
}

bool HostApp::show_about() {
    if (mode_ != UiMode::idle) {
        if (about_window_ && IsWindow(about_window_)) {
            SetForegroundWindow(about_window_);
        }
        return false;
    }

    mode_ = UiMode::about;
    unregister_hotkeys();
    about_window_ = show_about_window_async(window_, [this] {
        about_window_ = nullptr;
        if (mode_ == UiMode::shutting_down) {
            maybe_finish_shutdown();
            return;
        }
        mode_ = UiMode::idle;
        if (config_.shell_enabled) {
            register_hotkeys();
        }
    });
    return about_window_ != nullptr;
}

void HostApp::notify(std::wstring_view title, std::wstring_view message) {
    if (!tray_added_) {
        return;
    }
    const std::wstring title_text(title);
    const std::wstring message_text(message);
    tray_.uFlags = NIF_INFO;
    wcsncpy_s(tray_.szInfoTitle, title_text.c_str(), _TRUNCATE);
    wcsncpy_s(tray_.szInfo, message_text.c_str(), _TRUNCATE);
    tray_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

void HostApp::capture_region(RegionAction action) {
    if (mode_ != UiMode::idle) {
        return;
    }
    if (action == RegionAction::ocr && !config_.ocr_enabled) {
        notify(kAppName, L"OCR 模块已关闭。");
        return;
    }

    RegionRequest request;
    request.action = action;
    request.config = config_;
    request.copy_ocr = true;
    start_region_capture(std::move(request), nullptr, true);
}

std::optional<std::wstring> HostApp::sync_region_config(const RegionResult& result) {
    if (result.code == ExitCode::operation_failed && result.bounds.empty()) {
        return std::nullopt;
    }
    const AppConfig& next = result.config;
    const bool changed = config_.custom_color != next.custom_color ||
                         config_.annotation_locked_tool != next.annotation_locked_tool ||
                         config_.annotation_hidden_tools != next.annotation_hidden_tools ||
                         config_.annotation_highlight_alpha != next.annotation_highlight_alpha ||
                         config_.annotation_next_serial != next.annotation_next_serial;
    if (!changed) {
        return std::nullopt;
    }

    AppConfig updated = config_;
    updated.custom_color = next.custom_color;
    updated.annotation_locked_tool = next.annotation_locked_tool;
    updated.annotation_hidden_tools = next.annotation_hidden_tools;
    updated.annotation_highlight_alpha = next.annotation_highlight_alpha;
    updated.annotation_next_serial = next.annotation_next_serial;

    std::wstring error;
    if (!config_store_.save(updated, &error)) {
        if (error.empty()) {
            error = L"无法写入配置文件。";
        }
        return error;
    }
    config_ = std::move(updated);
    return std::nullopt;
}

void HostApp::check_for_updates(bool user_triggered) {
    bool expected = false;
    if (!update_running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    if (update_thread_.joinable()) {
        update_thread_.join();
    }

    const HWND owner = window_;
    update_thread_ = std::jthread([this, owner, user_triggered](std::stop_token stop_token) {
        UpdateCompletion completion;
        completion.user_triggered = user_triggered;
        try {
            const ScopedWinrtApartment apartment;
            if (!apartment.available()) {
                completion.message = L"无法初始化更新检查所需的 Windows 运行时。";
            } else {
                completion.result = stage_latest_update(&completion.message, stop_token);
            }
        } catch (const std::exception&) {
            completion.result = UpdateStageResult::failed;
            completion.message = L"检查更新时发生异常。";
        }

        if (stop_token.stop_requested()) {
            update_running_.store(false, std::memory_order_release);
            return;
        }
        {
            std::lock_guard lock(update_mutex_);
            update_completion_.emplace(std::move(completion));
        }
        if (!PostMessageW(owner, kUpdateCompleted, 0, 0)) {
            std::lock_guard lock(update_mutex_);
            update_completion_.reset();
            update_running_.store(false, std::memory_order_release);
        }
    });
}

std::wstring HostApp::handle_pipe_request(
    HWND dispatch_window,
    std::wstring_view request_text,
    const PipeRequestContext& pipe_context) {
    if (!accepting_requests_.load(std::memory_order_acquire)) {
        return serialize_response(shutting_down_response());
    }

    auto request = std::make_shared<RequestContext>();
    request->id = pipe_context.correlation_id();
    try {
        request->request.assign(request_text);
        if (transient_.load(std::memory_order_acquire)) {
            const std::wstring launch_nonce = launch_nonce_from_request(request_text);
            const bool belongs_to_launcher =
                !transient_launch_nonce_.empty() &&
                launch_nonce == transient_launch_nonce_;
            bool expected = false;
            if (belongs_to_launcher &&
                transient_primary_assigned_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                request->transient_generation.store(
                    transient_generation_.load(std::memory_order_acquire),
                    std::memory_order_release);
            }
        }
        std::future<std::wstring> response = request->response.get_future();

        {
            std::lock_guard lock(request_mutex_);
            if (!accepting_requests_.load(std::memory_order_acquire)) {
                return serialize_response(shutting_down_response());
            }
            pending_requests_.emplace(request->id, request);
            dispatch_queue_.push_back(request);
        }

        if (!PostMessageW(dispatch_window, kDispatchRequest, 0, 0)) {
            CommandResponse failure;
            failure.code = ExitCode::ipc_failed;
            failure.message = L"无法把请求投递到宿主 UI 线程。";
            failure.error_type = L"ipc_failed";
            complete_request(request, std::move(failure));
        }

        const auto deadline = std::chrono::steady_clock::now() + kRequestTimeout;
        while (response.wait_for(kRequestCancellationPoll) != std::future_status::ready) {
            CommandResponse abandoned;
            if (pipe_context.cancellation_requested()) {
                abandoned.code = ExitCode::ipc_failed;
                abandoned.message = L"请求客户端已断开连接。";
                abandoned.error_type = L"client_disconnected";
            } else if (std::chrono::steady_clock::now() >= deadline) {
                abandoned.code = ExitCode::operation_failed;
                abandoned.message = L"宿主操作超时。";
                abandoned.error_type = L"timeout";
            } else {
                continue;
            }

            request->cancelled.store(true, std::memory_order_release);
            bool expected = false;
            if (request->completed.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                {
                    std::lock_guard lock(request_mutex_);
                    pending_requests_.erase(request->id);
                }
                PostMessageW(
                    dispatch_window, kRequestTimedOut, static_cast<WPARAM>(request->id), 0);
                remember_shutdown_response(request);
                return serialize_response(abandoned);
            }
            response.wait();
            break;
        }

        try {
            std::wstring value = response.get();
            remember_shutdown_response(request);
            return value;
        } catch (...) {
            CommandResponse failure;
            failure.code = ExitCode::ipc_failed;
            failure.message = L"宿主未能完成请求。";
            failure.error_type = L"ipc_failed";
            remember_shutdown_response(request);
            return serialize_response(failure);
        }
    } catch (...) {
        request->cancelled.store(true, std::memory_order_release);
        request->completed.store(true, std::memory_order_release);
        {
            std::lock_guard lock(request_mutex_);
            pending_requests_.erase(request->id);
            std::erase(dispatch_queue_, request);
        }
        remember_shutdown_response(request);
        throw;
    }
}

void HostApp::dispatch_request(std::shared_ptr<RequestContext> request) {
    std::wstring error;
    const auto command = command_from_json(request->request, &error);
    if (!command) {
        CommandResponse response;
        response.code = ExitCode::invalid_arguments;
        response.message = error.empty() ? L"宿主请求无效。" : std::move(error);
        response.error_type = L"invalid_arguments";
        complete_request(request, std::move(response));
        return;
    }

    std::visit(
        [this, request = std::move(request)](const auto& value) {
            dispatch_command(value, request);
        },
        *command);
}

void HostApp::dispatch_command(const CaptureCommand& command, std::shared_ptr<RequestContext> request) {
    if (mode_ != UiMode::idle) {
        complete_request(request, busy_response());
        return;
    }

    if (command.mode == CaptureMode::region) {
        RegionRequest region;
        region.config = config_;
        region.path = command.path.wstring();
        switch (command.output) {
            case CaptureOutput::interactive: region.action = RegionAction::interactive; break;
            case CaptureOutput::clipboard: region.action = RegionAction::clipboard; break;
            case CaptureOutput::file: region.action = RegionAction::file; break;
        }
        start_region_capture(std::move(region), std::move(request), false);
        return;
    }

    if (command.mode == CaptureMode::window) {
        const auto captured = capture_active_window();
        if (!captured) {
            CommandResponse response;
            response.code = ExitCode::operation_failed;
            response.message = std::wstring(strings::capture_failed);
            complete_request(request, std::move(response));
            return;
        }
        complete_request(
            request,
            output_bitmap(
                std::move(captured->first), command.output, command.path, L"活动窗口截图完成。"));
        return;
    }

    const auto captured = capture_monitor(monitor_selector(command.monitor));
    if (!captured) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message = L"显示器不存在或截图失败。";
        complete_request(request, std::move(response));
        return;
    }
    complete_request(
        request,
        output_bitmap(std::move(captured->first), command.output, command.path, L"屏幕截图完成。"));
}

void HostApp::dispatch_command(const OcrCommand& command, std::shared_ptr<RequestContext> request) {
    if (mode_ != UiMode::idle) {
        complete_request(request, busy_response());
        return;
    }
    if (!config_.ocr_enabled) {
        CommandResponse response;
        response.code = ExitCode::module_unavailable;
        response.message = L"OCR 模块已关闭。";
        complete_request(request, std::move(response));
        return;
    }

    RegionRequest region;
    region.config = config_;
    region.action = RegionAction::ocr;
    region.copy_ocr = command.copy;
    start_region_capture(std::move(region), std::move(request), false);
}

void HostApp::dispatch_command(const ModuleCommand& command, std::shared_ptr<RequestContext> request) {
    if (command.action == ModuleAction::list) {
        CommandResponse response;
        response.message = L"模块状态读取完成。";
        response.data_json = module_status_json(config_);
        complete_request(request, std::move(response));
        return;
    }
    if (mode_ != UiMode::idle) {
        complete_request(request, busy_response());
        return;
    }
    if (!command.module) {
        CommandResponse response;
        response.code = ExitCode::invalid_arguments;
        response.message = L"缺少模块名。";
        complete_request(request, std::move(response));
        return;
    }

    AppConfig next = config_;
    const bool enabled = command.action == ModuleAction::enable;
    switch (*command.module) {
        case ModuleId::annotation: next.annotation_enabled = enabled; break;
        case ModuleId::ocr: next.ocr_enabled = enabled; break;
        case ModuleId::shell: next.shell_enabled = enabled; break;
    }

    std::wstring error;
    if (!commit_config(std::move(next), &error)) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message =
            error.empty() ? L"无法保存模块设置。" : std::move(error);
        complete_request(request, std::move(response));
        return;
    }

    if (!transient_.load(std::memory_order_acquire)) {
        apply_shell();
    }
    if (!config_.shell_enabled && !transient_.load(std::memory_order_acquire)) {
        request->explicit_shutdown_after_response.store(true, std::memory_order_release);
    }

    CommandResponse response;
    response.message = enabled ? L"模块已启用。" : L"模块已关闭。";
    complete_request(request, std::move(response));
}

void HostApp::dispatch_command(const AppCommand& command, std::shared_ptr<RequestContext> request) {
    if (command.action == AppAction::status || command.action == AppAction::start) {
        if (command.action == AppAction::start) {
            promote_to_persistent();
            request->transient_generation.store(0, std::memory_order_release);
        }
        CommandResponse response;
        response.message = L"Air Screenshot 正在运行。";
        response.data_json =
            app_status_json(config_, transient_.load(std::memory_order_acquire));
        complete_request(request, std::move(response));
        return;
    }
    if (command.action == AppAction::stop) {
        request->explicit_shutdown_after_response.store(true, std::memory_order_release);
        CommandResponse response;
        response.message = L"Air Screenshot 已退出。";
        complete_request(request, std::move(response));
        return;
    }
    if (mode_ != UiMode::idle) {
        complete_request(request, busy_response());
        return;
    }
    promote_to_persistent();
    request->transient_generation.store(0, std::memory_order_release);
    if (!show_settings()) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message = L"无法打开设置窗口。";
        complete_request(request, std::move(response));
        return;
    }

    CommandResponse response;
    response.message = L"已打开设置。";
    complete_request(request, std::move(response));
}

void HostApp::start_region_capture(
    RegionRequest region, std::shared_ptr<RequestContext> request, bool notify_user) {
    if (mode_ != UiMode::idle) {
        complete_request(request, busy_response());
        return;
    }

    mode_ = UiMode::capturing;
    capture_request_ = request;
    auto session = airshot::start_region_capture(
        std::move(region),
        [this, request = std::move(request), notify_user](RegionResult result) mutable {
            capture_completion_.emplace(CaptureCompletion{std::move(request), std::move(result), notify_user});
            PostMessageW(window_, kCaptureCompleted, 0, 0);
        });
    capture_session_ = std::move(session);
}

void HostApp::handle_capture_completion() {
    if (!capture_completion_) {
        return;
    }

    CaptureCompletion completion = std::move(*capture_completion_);
    capture_completion_.reset();
    capture_session_.reset();
    capture_request_.reset();

    if (mode_ != UiMode::shutting_down) {
        mode_ = UiMode::idle;
    }
    if (auto config_error = sync_region_config(completion.result)) {
        if (!completion.result.message.empty()) {
            completion.result.message += L" ";
        }
        completion.result.message += L"截图设置未保存：" + *config_error;
    }

    if (completion.result.code == ExitCode::success && completion.result.action == RegionAction::pin) {
        auto pin = PinWindow::create(instance_,
                                     window_,
                                     completion.result.bitmap,
                                     completion.result.bounds.left,
                                     completion.result.bounds.top);
        if (pin) {
            pin_windows_.push_back(std::move(pin));
        } else {
            completion.result.code = ExitCode::operation_failed;
            completion.result.message = L"无法创建贴图窗口。";
        }
    }

    if (completion.request) {
        CommandResponse response;
        response.code = completion.result.code;
        response.message = completion.result.message;
        response.path = completion.result.path;
        response.text = completion.result.text;
        complete_request(completion.request, std::move(response));
    }

    if (completion.notify_user) {
        if (completion.result.code == ExitCode::success) {
            if (config_.notifications_enabled && completion.result.action != RegionAction::pin) {
                notify(kAppName, completion.result.message);
            }
        } else if (completion.result.code != ExitCode::user_cancelled) {
            notify(kAppName, completion.result.message);
        }
    }

    maybe_finish_shutdown();
}

void HostApp::handle_update_completion() {
    std::optional<UpdateCompletion> completion;
    {
        std::lock_guard lock(update_mutex_);
        completion = std::move(update_completion_);
        update_completion_.reset();
    }
    update_running_.store(false, std::memory_order_release);
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
    if (!completion || mode_ == UiMode::shutting_down) {
        return;
    }

    const bool display_result =
        completion->user_triggered ||
        completion->message.find(L"当前目录不可写") != std::wstring::npos ||
        completion->message.find(L"只读") != std::wstring::npos;
    if (!display_result) {
        return;
    }
    const UINT icon =
        completion->result == UpdateStageResult::failed ? MB_ICONERROR : MB_ICONINFORMATION;
    MessageBoxW(window_, completion->message.c_str(), kAppName, MB_OK | icon);
}

void HostApp::complete_request(const std::shared_ptr<RequestContext>& request, CommandResponse response) {
    if (!request) {
        return;
    }

    bool expected = false;
    if (!request->completed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    try {
        std::wstring response_json = serialize_response(response);
        request->response.set_value(std::move(response_json));
    } catch (...) {
        try {
            request->response.set_exception(std::current_exception());
        } catch (const std::future_error&) {
        }
    }

    std::lock_guard lock(request_mutex_);
    pending_requests_.erase(request->id);
}

CommandResponse HostApp::busy_response() const {
    CommandResponse response;
    response.code = ExitCode::operation_failed;
    response.message = L"当前有前台操作正在进行。";
    response.error_type = L"busy";
    return response;
}

CommandResponse HostApp::output_bitmap(Bitmap bitmap,
                                       CaptureOutput output,
                                       const std::filesystem::path& requested_path,
                                       std::wstring_view success_message) {
    if (bitmap.empty()) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message = std::wstring(strings::capture_failed);
        return response;
    }

    std::wstring error;
    if (output == CaptureOutput::file) {
        const std::filesystem::path path = resolve_output_path(requested_path.wstring());
        if (!save_png(bitmap, path, &error)) {
            CommandResponse response;
            response.code = ExitCode::operation_failed;
            response.message = std::move(error);
            return response;
        }
        CommandResponse response;
        response.message = std::wstring(success_message);
        response.path = path.wstring();
        return response;
    }

    if (!copy_bitmap_to_clipboard(window_, bitmap, &error)) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message = std::move(error);
        return response;
    }
    CommandResponse response;
    response.message = std::wstring(success_message);
    return response;
}

LRESULT HostApp::handle_message(UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_PIN_WINDOW_CLOSED) {
        auto* pin_ptr = reinterpret_cast<PinWindow*>(l_param);
        std::erase_if(pin_windows_, [pin_ptr](const auto& pin) {
            return pin.get() == pin_ptr;
        });
        maybe_finish_shutdown();
        return 0;
    }
    if (message == kDispatchRequest) {
        drain_dispatch_queue();
        return 0;
    }
    if (message == kCaptureCompleted) {
        handle_capture_completion();
        return 0;
    }
    if (message == kRequestTimedOut) {
        if (capture_request_ && capture_request_->id == static_cast<std::uint64_t>(w_param) && capture_session_) {
            capture_session_->cancel();
        }
        return 0;
    }
    if (message == kResponseCompleted) {
        request_shutdown();
        return 0;
    }
    if (message == kTransportCompleted) {
        maybe_finish_shutdown();
        return 0;
    }
    if (message == kTransientResponseDelivered) {
        arm_transient_idle_shutdown(kTransientSuccessGraceMs);
        maybe_shutdown_idle_transient();
        return 0;
    }
    if (message == kTransientDeliveryFailed) {
        arm_transient_idle_shutdown(kTransientFailureTimeoutMs);
        maybe_shutdown_idle_transient();
        return 0;
    }
    if (message == kUpdateCompleted) {
        handle_update_completion();
        return 0;
    }
    if (message == kIpcListenerFaulted) {
        request_shutdown();
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
        switch (LOWORD(w_param)) {
            case kMenuCapture: capture_region(RegionAction::interactive); break;
            case kMenuSettings: (void)show_settings(); break;
            case kMenuUpdate: check_for_updates(true); break;
            case kMenuAbout: (void)show_about(); break;
            case kMenuCloseAllPins:
                for (const auto& pin : pin_windows_) {
                    pin->request_close();
                }
                prune_closed_pin_windows();
                break;
            case kMenuExit: request_shutdown(); break;
            default: break;
        }
        return 0;
    }
    if (message == WM_TIMER && w_param == kShutdownDrainTimer) {
        maybe_finish_shutdown();
        return 0;
    }
    if (message == WM_TIMER && w_param == kTransientIdleTimer) {
        maybe_shutdown_idle_transient();
        return 0;
    }
    if (message == WM_QUERYENDSESSION) {
        return TRUE;
    }
    if (message == WM_ENDSESSION && w_param != FALSE) {
        request_shutdown();
        return 0;
    }
    if (message == WM_CLOSE) {
        request_shutdown();
        return 0;
    }
    if (message == WM_DESTROY) {
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

    LRESULT result = 0;
    try {
        result =
            app ? app->handle_message(message, w_param, l_param)
                : DefWindowProcW(window, message, w_param, l_param);
    } catch (...) {
        PostQuitMessage(static_cast<int>(ExitCode::unknown_error));
    }
    if (message == WM_NCDESTROY && app) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (app->window_ == window) {
            app->window_ = nullptr;
        }
    }
    return result;
}

}  // namespace airshot
