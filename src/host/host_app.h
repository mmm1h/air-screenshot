#pragma once

#include "airshot/bitmap.h"
#include "airshot/command.h"
#include "airshot/config.h"
#include "airshot/ipc.h"
#include "airshot/overlay.h"
#include "airshot/portable.h"

#include "pin_window.h"

#include <shellapi.h>

#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace airshot {

class HostApp {
public:
    HostApp(HINSTANCE instance, bool transient, std::wstring transient_launch_nonce = {});
    ~HostApp();

    int run();
    [[nodiscard]] bool is_transient() const noexcept {
        return transient_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool update_helper_launched() const noexcept {
        return update_helper_launched_;
    }

private:
    enum class UiMode {
        idle,
        capturing,
        settings,
        about,
        shutting_down,
    };

    struct RequestContext {
        std::uint64_t id{};
        std::wstring request;
        std::optional<Command> command;
        std::wstring parse_error;
        bool waits_for_user_input{};
        std::promise<std::wstring> response;
        std::atomic_bool completed{false};
        std::atomic_bool cancelled{false};
        std::atomic_bool explicit_shutdown_after_response{false};
        std::atomic_uint64_t transient_generation{0};
    };

    struct ShutdownResponse {
        bool explicit_request{};
        std::uint64_t transient_generation{};
    };

    struct CaptureCompletion {
        std::shared_ptr<RequestContext> request;
        RegionResult result;
        bool notify_user{};
    };

    struct UpdateCompletion {
        UpdateStageResult result{UpdateStageResult::failed};
        std::wstring message;
        bool user_triggered{};
    };

    bool initialize();
    void shutdown();
    void request_shutdown();
    void maybe_finish_shutdown();
    void reject_pending_requests();
    void drain_dispatch_queue();
    void note_response_sent(HWND dispatch_window, std::uint64_t correlation_id, bool sent);
    void remember_shutdown_response(const std::shared_ptr<RequestContext>& request);
    void apply_shell();
    [[nodiscard]] bool sync_startup_task(
        const AppConfig& config,
        std::wstring* error = nullptr);
    [[nodiscard]] bool commit_config(AppConfig next, std::wstring* error = nullptr);
    void prune_closed_pin_windows();
    [[nodiscard]] std::optional<std::wstring> pin_capacity_error(
        const Bitmap& bitmap) const;
    void suspend_pins_for_capture();
    void resume_pins_after_capture();
    void add_tray();
    void remove_tray();
    [[nodiscard]] bool validate_hotkeys(
        const AppConfig& config,
        std::wstring* error = nullptr) const;
    [[nodiscard]] bool register_hotkeys(
        std::wstring* error = nullptr,
        bool notify_failure = true);
    void unregister_hotkeys();
    void show_tray_menu();
    bool show_settings();
    bool show_about();
    void notify(std::wstring_view title, std::wstring_view message);
    void capture_region(RegionAction action);
    [[nodiscard]] std::optional<std::wstring> sync_region_config(const RegionResult& result);
    void schedule_update_check(DWORD fallback_delay_ms);
    void toggle_automatic_updates();
    [[nodiscard]] bool persist_update_config(
        AppConfig next,
        bool report_failure);
    void check_for_updates(bool user_triggered);
    void activate_pending_update();
    [[nodiscard]] bool can_restart_for_update();
    std::wstring handle_pipe_request(
        HWND dispatch_window,
        std::wstring_view request,
        const PipeRequestContext& pipe_context);
    void dispatch_request(std::shared_ptr<RequestContext> request);
    void dispatch_command(const CaptureCommand& command, std::shared_ptr<RequestContext> request);
    void dispatch_command(const OcrCommand& command, std::shared_ptr<RequestContext> request);
    void dispatch_command(const PinCommand& command, std::shared_ptr<RequestContext> request);
    void dispatch_command(const ModuleCommand& command, std::shared_ptr<RequestContext> request);
    void dispatch_command(const AppCommand& command, std::shared_ptr<RequestContext> request);
    void start_region_capture(RegionRequest region,
                              std::shared_ptr<RequestContext> request,
                              bool notify_user);
    void handle_capture_completion();
    void handle_update_completion();
    void complete_request(const std::shared_ptr<RequestContext>& request, CommandResponse response);
    void arm_transient_idle_shutdown(DWORD delay_ms);
    void cancel_transient_idle_shutdown();
    void maybe_shutdown_idle_transient();
    void promote_to_persistent();
    [[nodiscard]] CommandResponse busy_response() const;
    [[nodiscard]] CommandResponse pin_clipboard();
    int restore_pin_interaction();
    CommandResponse output_bitmap(
        Bitmap bitmap,
        CaptureOutput output,
        const std::filesystem::path& requested_path,
        std::wstring_view success_message);

    LRESULT handle_message(UINT message, WPARAM w_param, LPARAM l_param);
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

    HINSTANCE instance_{};
    HWND window_{};
    std::atomic_bool transient_{false};
    bool tray_added_{};
    UINT taskbar_created_message_{};
    bool shutdown_complete_{};
    HANDLE mutex_{};
    UiMode mode_{UiMode::idle};
    AppConfig config_;
    ConfigStore config_store_;
    PipeServer pipe_server_;
    std::atomic_bool accepting_requests_{false};
    std::atomic_bool ipc_listener_faulted_{false};
    std::atomic_bool transient_primary_assigned_{false};
    std::atomic_uint64_t transient_generation_{1};
    std::atomic_uint32_t responses_in_flight_{0};
    std::wstring transient_launch_nonce_;
    ULONGLONG transient_idle_deadline_{};
    std::mutex request_mutex_;
    std::deque<std::shared_ptr<RequestContext>> dispatch_queue_;
    std::unordered_map<std::uint64_t, std::shared_ptr<RequestContext>> pending_requests_;
    std::mutex acknowledgement_mutex_;
    std::unordered_map<std::uint64_t, ShutdownResponse> shutdown_responses_;
    NOTIFYICONDATAW tray_{};
    std::unique_ptr<RegionCaptureSession> capture_session_;
    std::shared_ptr<RequestContext> capture_request_;
    std::optional<CaptureCompletion> capture_completion_;
    HWND settings_window_{};
    HWND about_window_{};
    std::jthread update_thread_;
    std::atomic_bool update_running_{false};
    std::mutex update_mutex_;
    std::optional<UpdateCompletion> update_completion_;
    bool update_ready_{};
    bool update_helper_launched_{};
    bool pins_suspended_for_capture_{};
    std::vector<std::unique_ptr<PinWindow>> pin_windows_;
};

}  // namespace airshot
