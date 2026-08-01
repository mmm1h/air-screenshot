#include "host_app.h"

#include "about_window.h"
#include "app_icon.h"
#include "settings_window.h"

#include "airshot/capture.h"
#include "airshot/clipboard.h"
#include "airshot/host_policy.h"
#include "airshot/output.h"
#include "airshot/pin_workflow.h"
#include "airshot/strings.h"
#include "airshot/update_policy.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <dwmapi.h>
#include <commdlg.h>

#include <chrono>
#include <array>
#include <exception>
#include <limits>
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
constexpr UINT kUpdateActivationCompleted = WM_APP + 10;
constexpr UINT kIpcListenerFaulted = WM_APP + 11;

constexpr int kHotkeyCapture = 1;
constexpr int kHotkeyOcr = 2;
constexpr int kHotkeyPin = 3;
constexpr int kHotkeyProbeCapture = 0x4151;
constexpr int kHotkeyProbeOcr = 0x4152;
constexpr int kHotkeyProbePin = 0x4153;

constexpr UINT kMenuCapture = 2001;
constexpr UINT kMenuSettings = 2002;
constexpr UINT kMenuUpdate = 2003;
constexpr UINT kMenuExit = 2004;
constexpr UINT kMenuCloseAllPins = 2005;
constexpr UINT kMenuAbout = 2006;
constexpr UINT kMenuAutomaticUpdates = 2007;
constexpr UINT kMenuRestorePins = 2008;
constexpr UINT kMenuPinClipboard = 2009;
constexpr UINT kMenuPinFile = 2010;
constexpr UINT kMenuHideAllPins = 2011;
constexpr UINT kMenuShowAllPins = 2012;
constexpr UINT kMenuRepeatCapture = 2013;
constexpr UINT kMenuShowRecentPin = 2014;
constexpr UINT_PTR kShutdownDrainTimer = 1;
constexpr UINT_PTR kTransientIdleTimer = 2;
constexpr UINT_PTR kUpdateTimer = 3;
constexpr UINT_PTR kTrayRetryTimer = 4;

constexpr auto kRequestTimeout = std::chrono::seconds(90);
constexpr auto kRequestCancellationPoll = std::chrono::milliseconds(50);
constexpr DWORD kTransientStartupTimeoutMs = 20000;
constexpr DWORD kTransientSuccessGraceMs = 500;
constexpr DWORD kTransientFailureTimeoutMs = 2000;
std::int64_t now_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

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
    data.SetNamedValue(
        L"trayIconVisible",
        JsonValue::CreateBooleanValue(config.tray_icon_visible));
    return data.Stringify().c_str();
}

std::wstring app_status_json(
    const AppConfig& config,
    bool transient,
    bool hotkeys_available,
    std::wstring_view runtime_error) {
    JsonObject data;
    data.SetNamedValue(L"running", JsonValue::CreateBooleanValue(true));
    data.SetNamedValue(L"transient", JsonValue::CreateBooleanValue(transient));
    data.SetNamedValue(L"shell", JsonValue::CreateBooleanValue(config.shell_enabled));
    data.SetNamedValue(
        L"trayIconVisible",
        JsonValue::CreateBooleanValue(config.tray_icon_visible));
    data.SetNamedValue(
        L"hotkeysAvailable",
        JsonValue::CreateBooleanValue(hotkeys_available));
    if (!runtime_error.empty()) {
        data.SetNamedValue(
            L"runtimeError",
            JsonValue::CreateStringValue(runtime_error));
    }
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

std::optional<std::filesystem::path> prompt_pin_image_path(HWND owner) {
    std::array<wchar_t, 32768> path{};
    constexpr wchar_t filter[] =
        L"图片文件\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp\0"
        L"所有文件\0*.*\0";
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = L"选择要贴出的图片";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                   OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }
    return std::filesystem::path(path.data());
}

std::optional<std::wstring> pin_budget_error_message(
    PinBudgetStatus status,
    bool replacing) {
    switch (status) {
        case PinBudgetStatus::allowed:
            return std::nullopt;
        case PinBudgetStatus::invalid_bitmap:
            return L"贴图图像为空或像素数据无效。";
        case PinBudgetStatus::pin_count_limit:
            return L"当前贴图已达到 32 张上限，请先销毁部分贴图。";
        case PinBudgetStatus::single_pin_limit:
            return L"单张贴图超过 64 MiB 像素安全上限，请先缩小图片。";
        case PinBudgetStatus::total_pin_limit:
            return replacing
                       ? L"替换后会超过 192 MiB 总像素安全上限，原贴图已保留。"
                       : L"创建该贴图会超过 192 MiB 总像素安全上限，请先销毁部分贴图。";
    }
    return L"贴图像素预算校验失败。";
}

}  // namespace

HostApp::HostApp(
    HINSTANCE instance, bool transient, std::wstring transient_launch_nonce)
    : instance_(instance),
      transient_(transient),
      transient_launch_nonce_(std::move(transient_launch_nonce)) {
    mutex_ = CreateMutexW(nullptr, TRUE, kHostMutexName);
    if (mutex_ && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}

HostApp::~HostApp() {
    shutdown();
    if (mutex_) {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}

int HostApp::run() {
    if (!initialize()) {
        return static_cast<int>(ExitCode::ipc_failed);
    }
    initialized_ = true;

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
    if (!mutex_) {
        return false;
    }

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = load_app_icon(
        instance_,
        kDefaultAppIcon,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON));
    window_class.hIconSm = load_app_icon(
        instance_,
        kDefaultAppIcon,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON));
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
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");

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
    apply_app_icon_to_window(window_, config_.app_icon);
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
        schedule_update_check(kUpdateInitialDelayMs);
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
        KillTimer(window_, kUpdateTimer);
        KillTimer(window_, kTrayRetryTimer);
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
    update_user_triggered_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(update_mutex_);
        update_completion_.reset();
    }
    if (update_activation_thread_.joinable()) {
        update_activation_thread_.join();
    }
    update_activation_running_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(update_activation_mutex_);
        update_activation_completion_.reset();
    }

    if (window_ && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    window_ = nullptr;
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
        KillTimer(window_, kUpdateTimer);
        KillTimer(window_, kTrayRetryTimer);
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
        update_activation_running_.load(std::memory_order_acquire) ||
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
    schedule_update_check(kUpdateInitialDelayMs);
}

void HostApp::apply_shell(bool hotkeys_already_registered) {
    if (!hotkeys_already_registered) {
        unregister_hotkeys();
    }
    remove_tray();
    if (config_.shell_enabled) {
        if (config_.tray_icon_visible) {
            add_tray();
        }
        if (!hotkeys_already_registered) {
            (void)register_hotkeys();
        }
    }
}

bool HostApp::sync_startup_task(const AppConfig& config, std::wstring* error) {
    return sync_portable_startup(config.shell_enabled && config.start_at_login, error);
}

bool HostApp::commit_config(AppConfig next, std::wstring* error) {
    if (error) {
        error->clear();
    }

    const bool startup_changed = startup_sync_required(
        config_.shell_enabled,
        config_.start_at_login,
        next.shell_enabled,
        next.start_at_login);

    std::wstring startup_error;
    if (startup_changed && !sync_startup_task(next, &startup_error)) {
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
        const bool startup_rolled_back =
            !startup_changed || sync_startup_task(config_, &rollback_error);
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
    apply_app_icon_to_window(window_, config_.app_icon);
    for (const auto& pin : pin_windows_) {
        if (pin) {
            pin->set_click_through_available(
                config_.tray_icon_visible);
        }
    }
    return true;
}

void HostApp::prune_closed_pin_windows() {
    std::erase_if(pin_windows_, [](const auto& pin) {
        const HWND hwnd = pin ? pin->hwnd() : nullptr;
        return !hwnd || !IsWindow(hwnd);
    });
}

std::size_t HostApp::total_pin_bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& pin : pin_windows_) {
        if (!pin) {
            continue;
        }
        const std::size_t bytes = pin->bitmap_bytes();
        if (bytes > std::numeric_limits<std::size_t>::max() - total) {
            return std::numeric_limits<std::size_t>::max();
        }
        total += bytes;
    }
    return total;
}

std::optional<std::wstring> HostApp::pin_capacity_error(
    const Bitmap& bitmap) const {
    const PinBudgetPlan plan = plan_pin_bitmap_change(
        pin_windows_.size(),
        total_pin_bytes(),
        0,
        bitmap.valid() ? bitmap.pixels.size() : 0);
    return pin_budget_error_message(plan.status, false);
}

std::optional<std::wstring> HostApp::pin_replacement_capacity_error(
    std::size_t current_bytes,
    const Bitmap& bitmap) const {
    const PinBudgetPlan plan = plan_pin_bitmap_change(
        pin_windows_.size(),
        total_pin_bytes(),
        current_bytes,
        bitmap.valid() ? bitmap.pixels.size() : 0);
    return pin_budget_error_message(plan.status, true);
}

std::unique_ptr<PinWindow> HostApp::create_pin_window(
    Bitmap bitmap,
    int x,
    int y) {
    return PinWindow::create(
        instance_,
        window_,
        std::move(bitmap),
        x,
        y,
        config_.tray_icon_visible,
        [this](std::size_t current_bytes, const Bitmap& replacement) {
            return pin_replacement_capacity_error(
                current_bytes,
                replacement);
        });
}

void HostApp::suspend_pins_for_capture() {
    if (pins_suspended_for_capture_) {
        return;
    }
    prune_closed_pin_windows();
    for (const auto& pin : pin_windows_) {
        if (pin) {
            pin->suspend_for_capture();
        }
    }
    pins_suspended_for_capture_ = true;
    (void)DwmFlush();
}

void HostApp::resume_pins_after_capture() {
    if (!pins_suspended_for_capture_) {
        return;
    }
    pins_suspended_for_capture_ = false;
    for (const auto& pin : pin_windows_) {
        if (pin) {
            pin->resume_after_capture();
        }
    }
    (void)DwmFlush();
}

void HostApp::add_tray() {
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = window_;
    tray_.uID = 1;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = kTrayMessage;
    const int icon_size = std::max(16, GetSystemMetrics(SM_CXSMICON));
    tray_.hIcon = load_app_icon(instance_, config_.app_icon, icon_size, icon_size);
    wcscpy_s(tray_.szTip, kAppName);
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &tray_) == TRUE;
    if (tray_added_) {
        tray_retry_count_ = 0;
        if (window_) {
            KillTimer(window_, kTrayRetryTimer);
        }
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    } else if (window_ && config_.shell_enabled &&
               config_.tray_icon_visible && tray_retry_count_ < 3) {
        ++tray_retry_count_;
        SetTimer(
            window_,
            kTrayRetryTimer,
            static_cast<UINT>(500U * tray_retry_count_),
            nullptr);
    }
}

void HostApp::remove_tray() {
    if (window_) {
        KillTimer(window_, kTrayRetryTimer);
    }
    tray_retry_count_ = 0;
    if (tray_added_) {
        Shell_NotifyIconW(NIM_DELETE, &tray_);
        tray_added_ = false;
    }
}

bool HostApp::validate_hotkeys(
    const AppConfig& config,
    std::wstring* error) const {
    if (error) {
        error->clear();
    }
    if (!window_ || !config.shell_enabled) {
        return true;
    }

    const auto fail = [error](std::wstring_view name, DWORD code) {
        if (error) {
            *error =
                std::wstring(name) +
                L"当前已被其他程序占用，请换一个组合。";
            if (code != ERROR_HOTKEY_ALREADY_REGISTERED &&
                code != ERROR_SUCCESS) {
                *error += L"\n\n系统信息：" + windows_error_message(code);
            }
        }
        return false;
    };

    const auto capture = parse_hotkey(config.capture_hotkey);
    if (!capture) {
        if (error) {
            *error = L"截图全局快捷键格式无效，请重新录入。";
        }
        return false;
    }
    if (!RegisterHotKey(
            window_,
            kHotkeyProbeCapture,
            capture->modifiers,
            capture->virtual_key)) {
        return fail(L"截图全局快捷键", GetLastError());
    }

    bool pin_registered = false;
    if (!config.pin_hotkey.empty()) {
        const auto pin = parse_hotkey(config.pin_hotkey);
        if (!pin) {
            UnregisterHotKey(window_, kHotkeyProbeCapture);
            if (error) {
                *error = L"剪贴板贴图快捷键格式无效，请重新录入。";
            }
            return false;
        }
        if (!RegisterHotKey(
                window_,
                kHotkeyProbePin,
                pin->modifiers,
                pin->virtual_key)) {
            const DWORD register_error = GetLastError();
            UnregisterHotKey(window_, kHotkeyProbeCapture);
            return fail(L"剪贴板贴图快捷键", register_error);
        }
        pin_registered = true;
    }

    bool ocr_registered = false;
    bool valid = true;
    if (config.ocr_enabled && config.global_ocr_enabled) {
        const auto ocr = parse_hotkey(config.global_ocr_hotkey);
        if (!ocr) {
            if (error) {
                *error = L"OCR 全局快捷键格式无效，请重新录入。";
            }
            valid = false;
        } else if (!RegisterHotKey(
                       window_,
                       kHotkeyProbeOcr,
                       ocr->modifiers,
                       ocr->virtual_key)) {
            const DWORD register_error = GetLastError();
            if (pin_registered) {
                UnregisterHotKey(window_, kHotkeyProbePin);
            }
            UnregisterHotKey(window_, kHotkeyProbeCapture);
            return fail(L"OCR 全局快捷键", register_error);
        } else {
            ocr_registered = true;
        }
    }

    if (ocr_registered) {
        UnregisterHotKey(window_, kHotkeyProbeOcr);
    }
    if (pin_registered) {
        UnregisterHotKey(window_, kHotkeyProbePin);
    }
    UnregisterHotKey(window_, kHotkeyProbeCapture);
    return valid;
}

bool HostApp::register_hotkeys(
    std::wstring* error,
    bool notify_failure,
    const AppConfig* config_override,
    bool allow_while_settings) {
    if (error) {
        error->clear();
    }
    const AppConfig& hotkey_config =
        config_override ? *config_override : config_;
    const bool mode_allows_registration =
        mode_ == UiMode::idle ||
        (allow_while_settings && mode_ == UiMode::settings);
    if (!window_ || !hotkey_config.shell_enabled) {
        hotkeys_registered_ = false;
        hotkey_runtime_error_.clear();
        return true;
    }
    if (!mode_allows_registration) {
        return true;
    }
    bool capture_registered = false;
    if (const auto capture = parse_hotkey(hotkey_config.capture_hotkey)) {
        if (!RegisterHotKey(window_, kHotkeyCapture, capture->modifiers, capture->virtual_key)) {
            if (error) {
                *error = L"截图全局快捷键注册失败：" +
                         windows_error_message(GetLastError());
            }
        } else {
            capture_registered = true;
        }
    } else if (error) {
        *error = L"截图全局快捷键格式无效。";
    }

    bool pin_registered = false;
    if ((!error || error->empty()) && !hotkey_config.pin_hotkey.empty()) {
        if (const auto pin = parse_hotkey(hotkey_config.pin_hotkey)) {
            if (!RegisterHotKey(
                    window_,
                    kHotkeyPin,
                    pin->modifiers,
                    pin->virtual_key)) {
                if (error) {
                    *error = L"剪贴板贴图快捷键注册失败：" +
                             windows_error_message(GetLastError());
                }
            } else {
                pin_registered = true;
            }
        } else if (error) {
            *error = L"剪贴板贴图快捷键格式无效。";
        }
    }

    bool ocr_registered = false;
    if ((!error || error->empty()) &&
        hotkey_config.ocr_enabled &&
        hotkey_config.global_ocr_enabled) {
        if (const auto ocr = parse_hotkey(hotkey_config.global_ocr_hotkey)) {
            if (!RegisterHotKey(window_, kHotkeyOcr, ocr->modifiers, ocr->virtual_key)) {
                if (error) {
                    *error = L"OCR 全局快捷键注册失败：" +
                             windows_error_message(GetLastError());
                }
            } else {
                ocr_registered = true;
            }
        } else if (error) {
            *error = L"OCR 全局快捷键格式无效。";
        }
    }

    const bool succeeded =
        capture_registered &&
        (hotkey_config.pin_hotkey.empty() || pin_registered) &&
        (!hotkey_config.ocr_enabled || !hotkey_config.global_ocr_enabled || ocr_registered);
    hotkeys_registered_ = succeeded;
    if (!succeeded) {
        UnregisterHotKey(window_, kHotkeyCapture);
        UnregisterHotKey(window_, kHotkeyOcr);
        UnregisterHotKey(window_, kHotkeyPin);
        hotkey_runtime_error_ =
            error && !error->empty()
                ? *error
                : std::wstring(strings::hotkey_conflict);
        if (notify_failure) {
            if (tray_added_) {
                notify(kAppName, hotkey_runtime_error_);
            } else {
                MessageBoxW(
                    window_,
                    (hotkey_runtime_error_ +
                     L"\n\n托盘图标当前不可见。请关闭占用快捷键的程序，或运行 AirScreenshot.exe app settings 修改快捷键。").c_str(),
                    L"Air Screenshot 快捷键不可用",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            }
        }
    } else {
        hotkey_runtime_error_.clear();
    }
    return succeeded;
}

void HostApp::unregister_hotkeys() {
    if (window_) {
        UnregisterHotKey(window_, kHotkeyCapture);
        UnregisterHotKey(window_, kHotkeyOcr);
        UnregisterHotKey(window_, kHotkeyPin);
    }
    hotkeys_registered_ = false;
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
    AppendMenuW(menu, MF_STRING, kMenuRepeatCapture, L"重复上次区域");
    const std::wstring pin_label =
        config_.pin_hotkey.empty()
            ? L"贴出剪贴板"
            : L"贴出剪贴板\t" + config_.pin_hotkey;
    AppendMenuW(
        menu,
        MF_STRING,
        kMenuPinClipboard,
        pin_label.c_str());
    AppendMenuW(menu, MF_STRING, kMenuPinFile, L"从文件贴图…");
    AppendMenuW(menu, MF_STRING, kMenuSettings, strings::tray_settings.data());
    UINT update_flags = MF_STRING;
    const wchar_t* update_label = strings::tray_update.data();
    if (update_activation_running_.load(std::memory_order_acquire)) {
        update_flags |= MF_GRAYED;
        update_label = L"正在准备重启更新…";
    } else if (update_running_.load(std::memory_order_acquire)) {
        update_flags |= MF_GRAYED;
        update_label = L"正在检查更新…";
    } else if (update_ready_) {
        update_label = L"立即重启并更新";
    }
    AppendMenuW(menu, update_flags, kMenuUpdate, update_label);
    AppendMenuW(
        menu,
        MF_STRING |
            (mode_ == UiMode::idle ? 0U : MF_GRAYED) |
            (config_.automatic_updates_enabled ? MF_CHECKED : MF_UNCHECKED),
        kMenuAutomaticUpdates,
        L"自动检查并下载更新");
    AppendMenuW(menu, MF_STRING, kMenuAbout, L"关于");
    if (!pin_windows_.empty()) {
        std::vector<PinStateView> pin_states;
        pin_states.reserve(pin_windows_.size());
        for (const auto& pin : pin_windows_) {
            if (pin) {
                pin_states.push_back({
                    pin->hidden(),
                    pin->click_through(),
                    false,
                });
            }
        }
        const PinStateCounts counts = summarize_pin_states(pin_states);
        const std::wstring count_label = std::format(
            L"贴图：可见 {} / 隐藏 {}",
            counts.visible,
            counts.hidden);
        AppendMenuW(
            menu,
            MF_STRING | MF_GRAYED,
            0,
            count_label.c_str());
        const bool has_click_through_pin =
            std::ranges::any_of(
                pin_windows_,
                [](const auto& pin) {
                    return pin && pin->click_through();
                });
        if (has_click_through_pin) {
            AppendMenuW(
                menu,
                MF_STRING,
                kMenuRestorePins,
                L"恢复所有贴图交互");
        }
        if (counts.visible > 0) {
            AppendMenuW(menu, MF_STRING, kMenuHideAllPins, L"隐藏所有贴图");
        }
        if (counts.hidden > 0) {
            AppendMenuW(
                menu,
                MF_STRING,
                kMenuShowRecentPin,
                L"显示最近隐藏的贴图");
            AppendMenuW(
                menu,
                MF_STRING,
                kMenuShowAllPins,
                L"显示所有隐藏贴图");
        }
        AppendMenuW(
            menu,
            MF_STRING |
                (pin_bulk_destroy_confirmation_active_
                     ? MF_GRAYED
                     : 0U),
            kMenuCloseAllPins,
            L"永久销毁所有贴图…");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, strings::tray_exit.data());

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window_);
    const UINT selected =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window_, nullptr);
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
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
                if (keep_settings_launch_transient(
                        transient_.load(std::memory_order_acquire),
                        config_.shell_enabled)) {
                    arm_transient_idle_shutdown(1);
                    maybe_shutdown_idle_transient();
                }
                return;
            }

            mode_ = UiMode::idle;
            if (transient_.load(std::memory_order_acquire)) {
                if (config_.shell_enabled) {
                    promote_to_persistent();
                } else {
                    arm_transient_idle_shutdown(1);
                    maybe_shutdown_idle_transient();
                }
                return;
            }

            // The submit validator has already registered the committed
            // hotkeys. Preserve that reservation while applying tray changes
            // so another process cannot steal the combination after Save.
            apply_shell(true);
            if (!config_.shell_enabled) {
                request_shutdown();
            }
        },
        [this](const AppConfig& candidate, std::wstring* error) {
            AppConfig next = candidate;
            next.last_update_check_unix =
                config_.last_update_check_unix;
            next.warned_update_target =
                config_.warned_update_target;
            const AppConfig previous = config_;
            const bool automatic_updates_changed =
                previous.automatic_updates_enabled !=
                next.automatic_updates_enabled;

            const auto restore_previous_hotkeys =
                [this, &previous](std::wstring* restore_error) {
                    unregister_hotkeys();
                    return register_hotkeys(
                        restore_error,
                        false,
                        &previous,
                        true);
                };
            const auto append_restore_failure =
                [error](std::wstring_view restore_error) {
                    if (!error) {
                        return;
                    }
                    if (!error->empty()) {
                        *error += L"\n\n";
                    }
                    *error += L"此前快捷键也无法恢复";
                    if (!restore_error.empty()) {
                        *error += L"：";
                        *error += restore_error;
                    } else {
                        *error += L"。";
                    }
                };

            // Keep the old shortcuts reserved throughout normal editing. The
            // only vacancy is the bounded switch performed synchronously by
            // Save; any failure restores the old set before the dialog returns.
            unregister_hotkeys();
            if (!validate_hotkeys(next, error)) {
                std::wstring restore_error;
                if (!restore_previous_hotkeys(&restore_error)) {
                    append_restore_failure(restore_error);
                }
                return false;
            }

            std::wstring hotkey_error;
            if (!register_hotkeys(
                    &hotkey_error,
                    false,
                    &next,
                    true)) {
                if (error) {
                    *error = hotkey_error.empty()
                                 ? std::wstring(strings::hotkey_conflict)
                                 : std::move(hotkey_error);
                }
                std::wstring restore_error;
                if (!restore_previous_hotkeys(&restore_error)) {
                    append_restore_failure(restore_error);
                }
                return false;
            }

            std::wstring commit_error;
            if (!commit_config(std::move(next), &commit_error)) {
                if (error) {
                    *error = commit_error.empty()
                                 ? L"无法保存 Air Screenshot 配置。"
                                 : std::move(commit_error);
                }
                std::wstring restore_error;
                if (!restore_previous_hotkeys(&restore_error)) {
                    append_restore_failure(restore_error);
                }
                return false;
            }
            if (automatic_updates_changed) {
                static_cast<void>(apply_automatic_update_preference(
                    previous.automatic_updates_enabled));
            }
            return true;
        });

    if (!settings_window_ && mode_ == UiMode::settings) {
        mode_ = UiMode::idle;
        if (keep_settings_launch_transient(
                transient_.load(std::memory_order_acquire),
                config_.shell_enabled)) {
            arm_transient_idle_shutdown(1);
            maybe_shutdown_idle_transient();
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
            (void)register_hotkeys();
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

CommandResponse HostApp::repeat_last_region(
    CaptureOutput output,
    const std::filesystem::path& requested_path) {
    std::vector<DisplayMonitorGeometry> topology;
    std::wstring topology_signature;
    RepeatRegionResolution resolved;
    try {
        topology = current_display_topology();
        topology_signature = display_topology_signature(topology);
        resolved = resolve_repeat_region(
            config_.last_region_capture,
            topology);
    } catch (...) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message = L"无法读取当前显示器拓扑。";
        return response;
    }
    if (!resolved) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        switch (resolved.status) {
            case RepeatRegionStatus::no_history:
                response.message = L"没有可重复的上次截图区域。请先完成一次区域截图。";
                break;
            case RepeatRegionStatus::invalid_history:
                response.message = L"上次截图区域记录无效，无法重复截图。";
                break;
            case RepeatRegionStatus::no_displays:
                response.message = L"无法读取当前显示器拓扑。";
                break;
            case RepeatRegionStatus::no_intersection:
                response.message = L"上次截图区域与当前显示区域完全不相交，无法重复截图。";
                break;
            case RepeatRegionStatus::success:
                break;
        }
        return response;
    }

    suspend_pins_for_capture();
    Bitmap bitmap;
    try {
        bitmap = capture_rect(
            resolved.bounds,
            config_.capture_cursor);
    } catch (...) {
        bitmap = {};
    }
    resume_pins_after_capture();

    CommandResponse response = output_bitmap(
        std::move(bitmap),
        output == CaptureOutput::interactive
            ? CaptureOutput::clipboard
            : output,
        requested_path,
        resolved.cropped
            ? L"上次截图区域超过当前显示区域，已裁剪并完成截图。"
            : resolved.topology_changed
            ? L"已按当前显示器布局重复上次区域。"
            : L"已重复上次区域截图。");
    if (response.code != ExitCode::success) {
        return response;
    }

    AppConfig next = config_;
    next.last_region_capture = LastRegionCapture{
        resolved.bounds,
        std::move(topology_signature)};
    std::wstring save_error;
    if (!config_store_.save(next, &save_error)) {
        response.message +=
            L" 区域历史未保存：" +
            (save_error.empty() ? std::wstring(L"无法写入配置文件。")
                                : save_error);
    } else {
        config_ = std::move(next);
    }
    return response;
}

std::optional<std::wstring> HostApp::sync_region_config(const RegionResult& result) {
    if (result.code == ExitCode::operation_failed && result.bounds.empty()) {
        return std::nullopt;
    }
    const AppConfig& next = result.config;
    const bool style_changed =
        config_.custom_color != next.custom_color ||
        config_.annotation_locked_tool != next.annotation_locked_tool ||
        config_.annotation_hidden_tools != next.annotation_hidden_tools ||
        config_.annotation_highlight_alpha != next.annotation_highlight_alpha ||
        config_.annotation_tool_styles != next.annotation_tool_styles ||
        config_.capture_corner_radius != next.capture_corner_radius;
    std::optional<LastRegionCapture> last_region =
        config_.last_region_capture;
    if (result.code == ExitCode::success &&
        result.bounds.width() >= 2 && result.bounds.height() >= 2 &&
        valid_display_topology_signature(result.topology_signature)) {
        last_region = LastRegionCapture{
            result.bounds,
            result.topology_signature};
    }
    const bool region_changed =
        last_region.has_value() != config_.last_region_capture.has_value() ||
        (last_region && config_.last_region_capture &&
         (last_region->bounds.left !=
              config_.last_region_capture->bounds.left ||
          last_region->bounds.top !=
              config_.last_region_capture->bounds.top ||
          last_region->bounds.right !=
              config_.last_region_capture->bounds.right ||
          last_region->bounds.bottom !=
              config_.last_region_capture->bounds.bottom ||
          last_region->topology_signature !=
              config_.last_region_capture->topology_signature));
    const bool changed = style_changed || region_changed;
    if (!changed) {
        return std::nullopt;
    }

    AppConfig updated = config_;
    updated.custom_color = next.custom_color;
    updated.annotation_locked_tool = next.annotation_locked_tool;
    updated.annotation_hidden_tools = next.annotation_hidden_tools;
    updated.annotation_highlight_alpha = next.annotation_highlight_alpha;
    updated.annotation_tool_styles = next.annotation_tool_styles;
    updated.capture_corner_radius = next.capture_corner_radius;
    updated.last_region_capture = std::move(last_region);

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

void HostApp::schedule_update_check(DWORD fallback_delay_ms) {
    if (!window_ || transient_.load(std::memory_order_acquire)) {
        return;
    }
    KillTimer(window_, kUpdateTimer);
    if (!config_.automatic_updates_enabled ||
        mode_ == UiMode::shutting_down) {
        return;
    }
    const DWORD delay = automatic_update_delay_ms(
        config_.last_update_check_unix,
        now_unix_seconds(),
        fallback_delay_ms);
    SetTimer(window_, kUpdateTimer, delay, nullptr);
}

bool HostApp::apply_automatic_update_preference(
    bool previous_enabled) {
    const AutomaticUpdateRuntimeAction action =
        automatic_update_runtime_action(
            previous_enabled,
            config_.automatic_updates_enabled,
            update_running_.load(std::memory_order_acquire),
            update_user_triggered_.load(std::memory_order_acquire));
    switch (action) {
        case AutomaticUpdateRuntimeAction::none:
            return false;
        case AutomaticUpdateRuntimeAction::schedule:
            schedule_update_check(kUpdateInitialDelayMs);
            return false;
        case AutomaticUpdateRuntimeAction::disable:
            if (window_) {
                KillTimer(window_, kUpdateTimer);
            }
            return false;
        case AutomaticUpdateRuntimeAction::disable_and_cancel:
            if (window_) {
                KillTimer(window_, kUpdateTimer);
            }
            if (update_thread_.joinable()) {
                update_thread_.request_stop();
                return true;
            }
            return false;
    }
    return false;
}

bool HostApp::persist_update_config(
    AppConfig next,
    bool report_failure) {
    std::wstring error;
    if (!config_store_.save(next, &error)) {
        if (report_failure) {
            notify(
                L"更新设置未保存",
                error.empty() ? L"无法保存自动更新设置。" : error);
        }
        return false;
    }
    config_ = std::move(next);
    return true;
}

void HostApp::toggle_automatic_updates() {
    const bool previous_enabled = config_.automatic_updates_enabled;
    AppConfig next = config_;
    next.automatic_updates_enabled =
        !next.automatic_updates_enabled;
    if (!persist_update_config(std::move(next), true)) {
        return;
    }

    const bool canceling_automatic_update =
        apply_automatic_update_preference(previous_enabled);
    if (config_.automatic_updates_enabled) {
        notify(
            L"自动更新已开启",
            L"Air Screenshot 将每天静默检查并下载经过验证的更新。");
    } else {
        notify(
            L"自动更新已关闭",
            canceling_automatic_update
                ? L"正在停止后台自动下载；自动暂存的版本也会暂停安装，仍可手动检查更新。"
                : L"自动暂存的版本会暂停安装；仍可从托盘菜单手动检查更新。");
    }
}

void HostApp::check_for_updates(bool user_triggered) {
    if (!user_triggered && !config_.automatic_updates_enabled) {
        return;
    }
    if (update_ready_) {
        if (user_triggered) {
            activate_pending_update();
        }
        return;
    }

    bool expected = false;
    if (!update_running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        if (user_triggered) {
            notify(L"正在检查更新", L"请等待当前更新检查完成。");
        }
        return;
    }

    std::wstring target_error;
    if (!update_target_is_replaceable(&target_error)) {
        const std::wstring target_key =
            update_target_key(portable_executable_path());
        const bool show_warning = should_show_update_target_warning(
            user_triggered,
            config_.warned_update_target,
            target_key);
        AppConfig next = config_;
        next.warned_update_target = target_key;
        if (!user_triggered) {
            next.last_update_check_unix = now_unix_seconds();
        }
        (void)persist_update_config(std::move(next), false);
        update_running_.store(false, std::memory_order_release);
        update_user_triggered_.store(false, std::memory_order_release);
        schedule_update_check(
            user_triggered
                ? kUpdateRetryDelayMs
                : kUpdateInitialDelayMs);
        if (show_warning) {
            notify(
                L"无法自动更新",
                target_error.empty()
                    ? L"当前程序位置不允许安全地替换可执行文件。"
                    : target_error);
        }
        return;
    }

    if (update_thread_.joinable()) {
        update_thread_.join();
    }

    const HWND owner = window_;
    update_user_triggered_.store(user_triggered, std::memory_order_release);
    try {
        update_thread_ = std::jthread(
            [this, owner, user_triggered](std::stop_token stop_token) {
                UpdateCompletion completion;
                completion.user_triggered = user_triggered;
                try {
                    const ScopedWinrtApartment apartment;
                    if (!apartment.available()) {
                        completion.message =
                            L"无法初始化更新检查所需的 Windows 运行时。";
                    } else {
                        completion.result =
                            stage_latest_update(
                                &completion.message,
                                stop_token,
                                user_triggered
                                    ? UpdateRequestSource::manual
                                    : UpdateRequestSource::automatic);
                    }
                } catch (const std::exception&) {
                    completion.result = UpdateStageResult::failed;
                    completion.message = L"检查更新时发生异常。";
                }

                completion.canceled = stop_token.stop_requested();
                {
                    std::lock_guard lock(update_mutex_);
                    update_completion_.emplace(std::move(completion));
                }
                if (!PostMessageW(owner, kUpdateCompleted, 0, 0)) {
                    std::lock_guard lock(update_mutex_);
                    update_completion_.reset();
                    update_running_.store(false, std::memory_order_release);
                    update_user_triggered_.store(false, std::memory_order_release);
                }
            });
    } catch (const std::exception&) {
        update_running_.store(false, std::memory_order_release);
        update_user_triggered_.store(false, std::memory_order_release);
        if (user_triggered) {
            notify(
                L"无法检查更新",
                L"无法启动后台更新检查线程。");
        }
        schedule_update_check(kUpdateRetryDelayMs);
        return;
    }

    if (user_triggered) {
        notify(
            L"正在检查更新",
            L"Air Screenshot 正在检查最新发布版本。");
    }
}

bool HostApp::can_restart_for_update() {
    prune_closed_pin_windows();
    if (update_activation_running_.load(std::memory_order_acquire) ||
        mode_ != UiMode::idle || capture_session_ ||
        capture_completion_ ||
        (settings_window_ && IsWindow(settings_window_)) ||
        (about_window_ && IsWindow(about_window_)) ||
        !pin_windows_.empty() ||
        pipe_server_.active_clients() != 0 ||
        responses_in_flight_.load(std::memory_order_acquire) != 0) {
        return false;
    }
    std::lock_guard lock(request_mutex_);
    return pending_requests_.empty() && dispatch_queue_.empty();
}

void HostApp::activate_pending_update() {
    std::wstring intent_error;
    switch (mark_pending_update_manual(&intent_error)) {
        case PendingUpdateManualResult::ready:
            break;
        case PendingUpdateManualResult::missing:
            update_ready_ = false;
            notify(
                L"待安装版本已不存在",
                L"请重新检查更新。");
            return;
        case PendingUpdateManualResult::invalid:
            update_ready_ = false;
            notify(
                L"待安装版本已失效",
                intent_error.empty()
                    ? L"请重新检查更新。"
                    : intent_error);
            return;
        case PendingUpdateManualResult::busy:
            notify(
                L"更新操作正在进行",
                L"手动更新入口已保留，请稍后重试。");
            return;
        case PendingUpdateManualResult::failed:
            notify(
                L"无法确认手动更新",
                intent_error.empty()
                    ? L"手动更新入口已保留，请稍后重试。"
                    : intent_error + L" 请稍后重试。");
            return;
    }
    if (!can_restart_for_update()) {
        notify(
            L"更新将在稍后安装",
            L"请先完成截图、设置、贴图或其他进行中的操作；也可直接退出，更新会在退出时应用。");
        return;
    }

    bool expected = false;
    if (!update_activation_running_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        return;
    }
    mode_ = UiMode::update_activation;

    if (update_activation_thread_.joinable()) {
        update_activation_thread_.join();
    }
    const HWND owner = window_;
    try {
        update_activation_thread_ = std::jthread([this, owner] {
            UpdateActivationCompletion completion;
            if (launch_pending_update(true, &completion.message)) {
                completion.result = UpdateActivationResult::launched;
            } else if (completion.message.empty()) {
                completion.result = UpdateActivationResult::unavailable;
            } else {
                completion.result = UpdateActivationResult::failed;
            }
            {
                std::lock_guard lock(update_activation_mutex_);
                update_activation_completion_.emplace(std::move(completion));
            }
            if (!PostMessageW(owner, kUpdateActivationCompleted, 0, 0)) {
                std::lock_guard lock(update_activation_mutex_);
                update_activation_completion_.reset();
                update_activation_running_.store(false, std::memory_order_release);
            }
        });
    } catch (const std::exception&) {
        update_activation_running_.store(false, std::memory_order_release);
        mode_ = UiMode::idle;
        notify(
            L"无法启动更新",
            L"无法创建后台更新应用任务，请稍后重试。");
        return;
    }
    notify(
        L"正在准备更新",
        L"Air Screenshot 正在后台启动已验证的更新，界面仍可正常响应。");
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
        request->command =
            command_from_json(request_text, &request->parse_error);
        request->waits_for_user_input =
            request->command &&
            command_waits_for_user_input(*request->command);
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
            } else if (!request->waits_for_user_input &&
                       std::chrono::steady_clock::now() >= deadline) {
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
    if (!request->command) {
        CommandResponse response;
        response.code = ExitCode::invalid_arguments;
        response.message = request->parse_error.empty()
                               ? L"宿主请求无效。"
                               : std::move(request->parse_error);
        response.error_type = L"invalid_arguments";
        complete_request(request, std::move(response));
        return;
    }

    Command command = std::move(*request->command);
    request->command.reset();
    std::visit(
        [this, request = std::move(request)](const auto& value) {
            dispatch_command(value, request);
        },
        command);
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

    if (command.mode == CaptureMode::repeat) {
        complete_request(
            request,
            repeat_last_region(command.output, command.path));
        return;
    }

    if (command.mode == CaptureMode::window) {
        suspend_pins_for_capture();
        const auto captured = [this]() noexcept {
            try {
                return capture_active_window(config_.capture_cursor);
            } catch (...) {
                return decltype(capture_active_window()){};
            }
        }();
        resume_pins_after_capture();
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

    suspend_pins_for_capture();
    const auto captured = [this, &command]() noexcept {
        try {
            return capture_monitor(
                monitor_selector(command.monitor),
                config_.capture_cursor);
        } catch (...) {
            return decltype(capture_monitor(
                monitor_selector(command.monitor))){};
        }
    }();
    resume_pins_after_capture();
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

void HostApp::dispatch_command(
    const PinCommand& command,
    std::shared_ptr<RequestContext> request) {
    if (command.action == PinAction::restore_interaction) {
        const int restored = restore_pin_interaction();
        CommandResponse response;
        response.message =
            restored > 0
                ? std::format(
                      L"已恢复 {} 张贴图的鼠标交互。",
                      restored)
                : L"没有需要恢复交互的贴图。";
        complete_request(request, std::move(response));
        return;
    }
    if (command.action == PinAction::toggle_interaction) {
        complete_request(request, toggle_pin_interaction());
        return;
    }
    if (mode_ != UiMode::idle) {
        complete_request(request, busy_response());
        return;
    }
    complete_request(
        request,
        command.action == PinAction::file
            ? pin_file(command.path)
            : pin_clipboard());
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
            app_status_json(
                config_,
                transient_.load(std::memory_order_acquire),
                !config_.shell_enabled || hotkeys_registered_,
                hotkey_runtime_error_);
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
    if (command.action == AppAction::tray_show) {
        if (mode_ != UiMode::idle) {
            complete_request(request, busy_response());
            return;
        }
        CommandResponse response;
        if (!config_.shell_enabled) {
            response.code = ExitCode::operation_failed;
            response.message =
                L"后台服务已关闭。请先运行 app settings 启用后台运行。";
            complete_request(request, std::move(response));
            return;
        }
        if (!config_.tray_icon_visible) {
            AppConfig next = config_;
            next.tray_icon_visible = true;
            std::wstring error;
            if (!commit_config(std::move(next), &error)) {
                response.code = ExitCode::operation_failed;
                response.message = error.empty()
                                       ? L"无法保存托盘图标设置。"
                                       : std::move(error);
                complete_request(request, std::move(response));
                return;
            }
        }
        if (transient_.load(std::memory_order_acquire)) {
            promote_to_persistent();
            request->transient_generation.store(0, std::memory_order_release);
        } else {
            apply_shell();
        }
        response.message = tray_added_
                               ? L"系统托盘图标已恢复。"
                               : L"已请求恢复系统托盘图标；Explorer 可用后会自动重试。";
        complete_request(request, std::move(response));
        return;
    }
    if (mode_ != UiMode::idle) {
        complete_request(request, busy_response());
        return;
    }
    // A settings-only launch must remain transient while shell integration is
    // disabled. Otherwise cancelling the dialog leaves an invisible host with
    // neither tray nor hotkeys. Saving shell=true promotes it in the completion
    // callback after the configuration transaction succeeds.
    if (config_.shell_enabled) {
        promote_to_persistent();
        request->transient_generation.store(0, std::memory_order_release);
    }
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
    suspend_pins_for_capture();
    std::unique_ptr<RegionCaptureSession> session;
    try {
        session = airshot::start_region_capture(
            std::move(region),
            [this, request = std::move(request), notify_user](RegionResult result) mutable {
                capture_completion_.emplace(
                    CaptureCompletion{
                        std::move(request),
                        std::move(result),
                        notify_user});
                PostMessageW(window_, kCaptureCompleted, 0, 0);
            });
    } catch (...) {
        session.reset();
    }
    if (!session && !capture_completion_) {
        auto failed_request = std::move(capture_request_);
        capture_request_.reset();
        resume_pins_after_capture();
        mode_ = UiMode::idle;
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message = L"无法启动截图界面，请稍后重试。";
        complete_request(failed_request, response);
        if (notify_user) {
            notify(kAppName, response.message);
        }
        return;
    }
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
    resume_pins_after_capture();

    if (mode_ != UiMode::shutting_down) {
        mode_ = UiMode::idle;
    }
    if (completion.result.code == ExitCode::success && completion.result.action == RegionAction::pin) {
        prune_closed_pin_windows();
        const auto capacity_error =
            pin_capacity_error(completion.result.bitmap);
        std::unique_ptr<PinWindow> pin;
        if (!capacity_error) {
            pin = create_pin_window(
                completion.result.bitmap,
                completion.result.bounds.left,
                completion.result.bounds.top);
        }
        if (pin) {
            pin_windows_.push_back(std::move(pin));
        } else {
            std::wstring clipboard_error;
            const bool copied = copy_bitmap_to_clipboard(
                window_,
                completion.result.bitmap,
                &clipboard_error);
            completion.result.code = ExitCode::operation_failed;
            const std::wstring reason =
                capacity_error
                    ? *capacity_error
                    : L"无法创建贴图窗口。";
            completion.result.message =
                copied
                    ? reason + L" 截图已保底复制到剪贴板。"
                    : (clipboard_error.empty()
                           ? reason + L" 也无法将截图复制到剪贴板。"
                           : reason + L" 剪贴板保底也失败：" +
                                 clipboard_error);
        }
    }

    if (auto config_error = sync_region_config(completion.result)) {
        if (!completion.result.message.empty()) {
            completion.result.message += L" ";
        }
        completion.result.message += L"截图设置未保存：" + *config_error;
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
    update_user_triggered_.store(false, std::memory_order_release);
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
    if (!completion || mode_ == UiMode::shutting_down) {
        return;
    }
    if (completion->canceled) {
        schedule_update_check(1'000);
        return;
    }

    if (completion->result != UpdateStageResult::failed) {
        AppConfig next = config_;
        next.warned_update_target.clear();
        next.last_update_check_unix = now_unix_seconds();
        (void)persist_update_config(std::move(next), false);
    }

    if (completion->result == UpdateStageResult::failed) {
        schedule_update_check(kUpdateRetryDelayMs);
    } else {
        schedule_update_check(
            static_cast<DWORD>(kUpdateIntervalSeconds * 1'000));
    }

    switch (completion->result) {
        case UpdateStageResult::staged:
            update_ready_ = true;
            if (!completion->user_triggered &&
                !config_.automatic_updates_enabled) {
                notify(
                    L"更新已下载但自动安装已暂停",
                    L"可从托盘菜单选择“立即重启并更新”，或重新开启自动更新。");
            } else {
                notify(
                    L"更新已准备好",
                    completion->user_triggered
                        ? L"可从托盘菜单选择“立即重启并更新”；继续使用时，更新会在退出或下次启动时安装。"
                        : L"新版本已通过校验，将在退出或下次启动时安装。");
            }
            break;
        case UpdateStageResult::up_to_date:
            if (completion->user_triggered) {
                notify(
                    L"Air Screenshot 已是最新版本",
                    completion->message.empty()
                        ? L"当前没有可用更新。"
                        : completion->message);
            }
            break;
        case UpdateStageResult::failed:
            if (completion->user_triggered) {
                notify(
                    L"检查更新失败",
                    completion->message.empty()
                        ? L"暂时无法检查更新，请稍后重试。"
                        : completion->message);
            }
            break;
    }
}

void HostApp::handle_update_activation_completion() {
    std::optional<UpdateActivationCompletion> completion;
    {
        std::lock_guard lock(update_activation_mutex_);
        completion = std::move(update_activation_completion_);
        update_activation_completion_.reset();
    }
    update_activation_running_.store(false, std::memory_order_release);
    if (update_activation_thread_.joinable()) {
        update_activation_thread_.join();
    }
    if (!completion) {
        return;
    }

    if (mode_ == UiMode::shutting_down) {
        if (completion->result == UpdateActivationResult::launched) {
            update_ready_ = false;
            update_helper_launched_ = true;
        }
        maybe_finish_shutdown();
        return;
    }

    if (mode_ == UiMode::update_activation) {
        mode_ = UiMode::idle;
    }

    switch (completion->result) {
        case UpdateActivationResult::launched:
            update_ready_ = false;
            update_helper_launched_ = true;
            request_shutdown();
            break;
        case UpdateActivationResult::unavailable:
            update_ready_ = false;
            check_for_updates(true);
            break;
        case UpdateActivationResult::failed:
            notify(
                L"无法启动更新",
                completion->message.empty()
                    ? L"已验证的更新暂时无法启动。"
                    : completion->message);
            break;
    }
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

CommandResponse HostApp::pin_bitmap(
    Bitmap bitmap,
    std::wstring_view description) {
    prune_closed_pin_windows();
    if (const auto capacity_error =
            pin_capacity_error(bitmap)) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message = *capacity_error;
        return response;
    }

    POINT position{};
    if (!GetCursorPos(&position)) {
        position = {
            GetSystemMetrics(SM_XVIRTUALSCREEN) + 48,
            GetSystemMetrics(SM_YVIRTUALSCREEN) + 48,
        };
    }
    const int cascade =
        static_cast<int>(pin_windows_.size() % 6U) * 18;
    position.x += cascade;
    position.y += cascade;

    auto pin = create_pin_window(
        std::move(bitmap),
        position.x,
        position.y);
    if (!pin) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message = L"剪贴板内容已读取，但无法创建贴图窗口。";
        return response;
    }
    pin_windows_.push_back(std::move(pin));

    CommandResponse response;
    response.message =
        description.empty()
            ? L"已创建贴图。"
            : L"已贴出" + std::wstring(description) + L"。";
    return response;
}

CommandResponse HostApp::pin_clipboard() {
    std::wstring error;
    auto visual = read_clipboard_visual(window_, &error);
    if (!visual || !visual->bitmap.valid()) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message =
            error.empty()
                ? L"剪贴板内容无法创建贴图。"
                : std::move(error);
        return response;
    }
    return pin_bitmap(
        std::move(visual->bitmap),
        visual->description.empty()
            ? std::wstring_view{L"剪贴板内容"}
            : std::wstring_view{visual->description});
}

CommandResponse HostApp::pin_file(
    const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute()) {
        CommandResponse response;
        response.code = ExitCode::invalid_arguments;
        response.message = L"贴图文件路径必须是绝对路径。";
        return response;
    }
    std::wstring error;
    const std::filesystem::path normalized = path.lexically_normal();
    auto bitmap = decode_local_image_file(normalized, &error);
    if (!bitmap) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message = error.empty()
                               ? L"无法读取要贴出的图片文件。"
                               : std::move(error);
        return response;
    }
    const std::wstring description = normalized.filename().empty()
                                         ? L"图片文件"
                                         : normalized.filename().wstring();
    return pin_bitmap(std::move(*bitmap), description);
}

CommandResponse HostApp::toggle_pin_interaction() {
    prune_closed_pin_windows();
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        CommandResponse response;
        response.code = ExitCode::operation_failed;
        response.message = L"无法读取鼠标位置。";
        return response;
    }

    struct OrderedPin {
        std::size_t rank{};
        PinWindow* pin{};
    };
    std::vector<OrderedPin> ordered;
    ordered.reserve(pin_windows_.size());
    for (const auto& pin : pin_windows_) {
        if (!pin || !pin->hwnd() || !IsWindow(pin->hwnd())) {
            continue;
        }
        std::size_t rank = 0;
        for (HWND above = GetWindow(pin->hwnd(), GW_HWNDPREV);
             above;
             above = GetWindow(above, GW_HWNDPREV)) {
            ++rank;
        }
        ordered.push_back({rank, pin.get()});
    }
    std::ranges::sort(
        ordered,
        [](const OrderedPin& left, const OrderedPin& right) {
            return left.rank < right.rank;
        });

    std::vector<PinStateView> states;
    states.reserve(ordered.size());
    for (const OrderedPin& item : ordered) {
        RECT bounds{};
        const bool visible =
            !item.pin->hidden() &&
            IsWindowVisible(item.pin->hwnd()) &&
            GetWindowRect(item.pin->hwnd(), &bounds);
        states.push_back({
            !visible,
            item.pin->click_through(),
            visible && PtInRect(&bounds, cursor),
        });
    }

    const PinTogglePlan plan = plan_pin_toggle(states);
    if (plan.action == PinToggleAction::toggle_target &&
        plan.target_index < ordered.size()) {
        PinWindow* target = ordered[plan.target_index].pin;
        const bool enabling = !target->click_through();
        if (!target->set_click_through(enabling)) {
            CommandResponse response;
            response.code = ExitCode::operation_failed;
            response.message =
                enabling && !config_.tray_icon_visible
                    ? L"托盘图标隐藏时不能开启鼠标穿透；请先在设置中显示托盘图标。"
                    : L"无法更改光标下贴图的鼠标穿透状态。";
            return response;
        }
        if (enabling) {
            PostMessageW(window_, WM_PIN_CLICK_THROUGH_ENABLED, 0, 0);
        }
        CommandResponse response;
        response.message = enabling
                               ? L"已开启光标下贴图的鼠标穿透。"
                               : L"已关闭光标下贴图的鼠标穿透。";
        return response;
    }

    const int restored = restore_pin_interaction();
    const int shown = show_most_recent_hidden_pin();
    CommandResponse response;
    if (restored > 0 && shown > 0) {
        response.message = std::format(
            L"光标下没有贴图，已恢复 {} 张贴图的鼠标交互，并显示最近隐藏的贴图。",
            restored);
    } else if (restored > 0) {
        response.message = std::format(
            L"光标下没有贴图，已恢复 {} 张贴图的鼠标交互。",
            restored);
    } else if (shown > 0) {
        response.message = L"光标下没有贴图，已显示最近隐藏的贴图。";
    } else {
        response.message =
            L"光标下没有贴图，也没有需要恢复的贴图。";
    }
    return response;
}

int HostApp::restore_pin_interaction() {
    prune_closed_pin_windows();
    int restored = 0;
    for (const auto& pin : pin_windows_) {
        if (pin && pin->click_through() &&
            pin->set_click_through(false)) {
            pin->ensure_visible();
            ++restored;
        }
    }
    return restored;
}

int HostApp::hide_all_pins() {
    prune_closed_pin_windows();
    int hidden_count = 0;
    for (const auto& pin : pin_windows_) {
        if (pin && !pin->hidden()) {
            pin->request_hide();
            ++hidden_count;
        }
    }

    return hidden_count;
}

int HostApp::show_most_recent_hidden_pin() {
    prune_closed_pin_windows();
    PinWindow* most_recent = nullptr;
    std::uint64_t most_recent_order = 0;
    for (const auto& pin : pin_windows_) {
        if (!pin || !pin->hidden()) {
            continue;
        }
        if (!most_recent || pin->hidden_order() > most_recent_order) {
            most_recent = pin.get();
            most_recent_order = pin->hidden_order();
        }
    }
    if (!most_recent) {
        return 0;
    }
    most_recent->request_show();
    return 1;
}

int HostApp::show_all_pins() {
    prune_closed_pin_windows();
    int shown_count = 0;
    for (const auto& pin : pin_windows_) {
        if (pin && pin->hidden()) {
            pin->request_show();
            ++shown_count;
        }
    }
    return shown_count;
}

bool HostApp::confirm_destroy_all_pins(std::size_t pin_count) const {
    if (pin_count == 0) {
        return false;
    }
    const std::wstring message = std::format(
        L"要永久销毁全部 {} 张贴图吗？\n\n销毁后无法从托盘恢复。",
        pin_count);
    return MessageBoxW(
               window_,
               message.c_str(),
               L"永久销毁全部贴图",
               MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
                   MB_SETFOREGROUND) == IDYES;
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
    if (taskbar_created_message_ != 0 &&
        message == taskbar_created_message_) {
        tray_added_ = false;
        if (config_.shell_enabled && config_.tray_icon_visible) {
            add_tray();
        }
        return 0;
    }
    if (message == WM_PIN_WINDOW_CLOSED) {
        const std::uint64_t pin_identifier =
            static_cast<std::uint64_t>(l_param);
        std::erase_if(pin_windows_, [pin_identifier](const auto& pin) {
            return pin && pin->identifier() == pin_identifier;
        });
        maybe_finish_shutdown();
        return 0;
    }
    if (message == WM_PIN_CLICK_THROUGH_ENABLED) {
        notify(
            L"贴图已开启鼠标穿透",
            L"可从托盘菜单选择“恢复所有贴图交互”，也可运行 pin restore。");
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
    if (message == kUpdateActivationCompleted) {
        handle_update_activation_completion();
        return 0;
    }
    if (message == kIpcListenerFaulted) {
        request_shutdown();
        return 0;
    }
    if (message == WM_DISPLAYCHANGE ||
        (message == WM_SETTINGCHANGE &&
         w_param == SPI_SETWORKAREA)) {
        for (const auto& pin : pin_windows_) {
            if (pin) {
                pin->ensure_visible();
            }
        }
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
        } else if (w_param == kHotkeyPin) {
            if (mode_ != UiMode::idle) {
                notify(kAppName, L"请先结束当前前台操作，再贴出剪贴板内容。");
            } else {
                const CommandResponse response = pin_clipboard();
                if (response.code != ExitCode::success) {
                    notify(
                        L"无法贴出剪贴板",
                        response.message.empty()
                            ? L"剪贴板中没有可贴出的内容。"
                            : response.message);
                }
            }
        }
        return 0;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(w_param)) {
            case kMenuCapture: capture_region(RegionAction::interactive); break;
            case kMenuRepeatCapture: {
                if (mode_ != UiMode::idle) {
                    notify(kAppName, L"请先结束当前前台操作，再重复上次区域。");
                    break;
                }
                const CommandResponse response = repeat_last_region(
                    CaptureOutput::clipboard);
                if (response.code != ExitCode::success ||
                    config_.notifications_enabled) {
                    notify(
                        response.code == ExitCode::success
                            ? kAppName
                            : L"无法重复上次区域",
                        response.message);
                }
                break;
            }
            case kMenuPinClipboard: {
                if (mode_ != UiMode::idle) {
                    notify(kAppName, L"请先结束当前前台操作，再贴出剪贴板内容。");
                    break;
                }
                const CommandResponse response = pin_clipboard();
                if (response.code != ExitCode::success) {
                    notify(
                        L"无法贴出剪贴板",
                        response.message.empty()
                            ? L"剪贴板中没有可贴出的内容。"
                            : response.message);
                }
                break;
            }
            case kMenuPinFile: {
                if (mode_ != UiMode::idle) {
                    notify(kAppName, L"请先结束当前前台操作，再从文件创建贴图。");
                    break;
                }
                mode_ = UiMode::pin_file_dialog;
                const auto path = prompt_pin_image_path(window_);
                CommandResponse response;
                if (path && mode_ != UiMode::shutting_down) {
                    response = pin_file(*path);
                }
                if (mode_ != UiMode::shutting_down) {
                    mode_ = UiMode::idle;
                }
                if (path && response.code != ExitCode::success) {
                    notify(
                        L"无法从文件创建贴图",
                        response.message.empty()
                            ? L"图片文件无法读取。"
                            : response.message);
                }
                break;
            }
            case kMenuSettings: (void)show_settings(); break;
            case kMenuUpdate:
                if (update_ready_) {
                    activate_pending_update();
                } else {
                    check_for_updates(true);
                }
                break;
            case kMenuAutomaticUpdates:
                // Do not let the tray mutate the same preference while the
                // settings window owns an isolated draft. Saving that draft
                // must never silently overwrite a concurrent tray change.
                if (mode_ == UiMode::idle) {
                    toggle_automatic_updates();
                } else if (mode_ == UiMode::settings && settings_window_ &&
                           IsWindow(settings_window_)) {
                    SetForegroundWindow(settings_window_);
                }
                break;
            case kMenuAbout: (void)show_about(); break;
            case kMenuCloseAllPins: {
                prune_closed_pin_windows();
                if (!can_begin_pin_bulk_destroy(
                        pin_bulk_destroy_confirmation_active_,
                        pin_windows_.size())) {
                    break;
                }
                std::vector<std::uint64_t> identifiers;
                identifiers.reserve(pin_windows_.size());
                for (const auto& pin : pin_windows_) {
                    if (pin) {
                        identifiers.push_back(pin->identifier());
                    }
                }

                pin_bulk_destroy_confirmation_active_ = true;
                bool confirmed = false;
                try {
                    confirmed = confirm_destroy_all_pins(
                        identifiers.size());
                } catch (...) {
                    pin_bulk_destroy_confirmation_active_ = false;
                    throw;
                }
                pin_bulk_destroy_confirmation_active_ = false;

                if (confirmed) {
                    for (const auto& pin : pin_windows_) {
                        if (pin && pin_destroy_snapshot_contains(
                                       identifiers,
                                       pin->identifier())) {
                            pin->request_close();
                        }
                    }
                    prune_closed_pin_windows();
                }
                break;
            }
            case kMenuHideAllPins:
                (void)hide_all_pins();
                break;
            case kMenuShowRecentPin:
                (void)show_most_recent_hidden_pin();
                break;
            case kMenuShowAllPins:
                (void)show_all_pins();
                break;
            case kMenuRestorePins: {
                const int restored = restore_pin_interaction();
                notify(
                    kAppName,
                    restored > 0
                        ? L"已恢复所有贴图的鼠标交互。"
                        : L"没有需要恢复的贴图。");
                break;
            }
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
    if (message == WM_TIMER && w_param == kUpdateTimer) {
        KillTimer(window_, kUpdateTimer);
        check_for_updates(false);
        return 0;
    }
    if (message == WM_TIMER && w_param == kTrayRetryTimer) {
        KillTimer(window_, kTrayRetryTimer);
        if (!tray_added_ && config_.shell_enabled &&
            config_.tray_icon_visible && mode_ != UiMode::shutting_down) {
            add_tray();
        }
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
