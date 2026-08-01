#include "host_app.h"
#include "cli_app.h"
#include "airshot/host_policy.h"
#include "airshot/ocr.h"

#include "airshot/portable.h"

#include <shellapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <utility>

namespace {

std::vector<std::wstring> command_arguments() {
    int count = 0;
    wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> result;
    for (int index = 1; values && index < count; ++index) {
        result.emplace_back(values[index]);
    }
    if (values) {
        LocalFree(values);
    }
    return result;
}

bool valid_launch_nonce(std::wstring_view value) {
    return value.size() == 32 &&
           std::ranges::all_of(value, [](wchar_t character) {
               return (character >= L'0' && character <= L'9') ||
                      (character >= L'a' && character <= L'f');
           });
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto arguments = command_arguments();
    if (!arguments.empty() && arguments[0] == L"--ocr-warm-smoke") {
        return airshot::run_ocr_warm_smoke(arguments);
    }
    if (!arguments.empty() && arguments[0] == L"--ocr-internal") {
        return airshot::run_ocr_cli(arguments);
    }
    if (!arguments.empty() &&
        arguments[0] == L"--verify-ocr-manifest") {
        return airshot::run_ocr_manifest_verifier(arguments);
    }
    if (!arguments.empty() &&
        (arguments[0] == L"--apply-update" || arguments[0] == L"--verify-update" ||
         arguments[0] == L"--check-update-target")) {
        return airshot::run_update_helper(arguments);
    }
    bool transient = false;
    std::wstring transient_launch_nonce;
    if (!arguments.empty() && arguments[0].starts_with(L"--transient=")) {
        transient_launch_nonce =
            arguments[0].substr(std::wstring_view(L"--transient=").size());
        transient = valid_launch_nonce(transient_launch_nonce);
    }
    if (!arguments.empty() && !transient) {
        return airshot::run_cli(arguments);
    }

    const airshot::ScopedWinrtApartment ui_apartment(true);
    if (!ui_apartment.available()) {
        if (!transient) {
            MessageBoxW(nullptr,
                        L"无法初始化 Air Screenshot 的 UI 线程。",
                        airshot::kAppName,
                        MB_OK | MB_ICONERROR);
        }
        return static_cast<int>(airshot::ExitCode::unknown_error);
    }
    airshot::HostApp app(
        instance, transient, std::move(transient_launch_nonce));
    if (!app.owns_host_instance()) {
        return static_cast<int>(airshot::ExitCode::ipc_failed);
    }
    if (airshot::check_pending_update_on_host_startup(
            app.owns_host_instance(), transient)) {
        std::wstring config_error;
        const auto startup_config =
            airshot::ConfigStore().load(&config_error);
        const bool allow_automatic_pending =
            startup_config && !startup_config->write_protected &&
            startup_config->automatic_updates_enabled;
        std::wstring update_error;
        if (airshot::launch_pending_update(
                true,
                allow_automatic_pending,
                &update_error)) {
            return 0;
        }
        airshot::cleanup_stale_updates();
    }
    const int result = app.run();
    if (airshot::launch_pending_update_on_host_exit(
            app.initialized(),
            app.is_transient(),
            app.update_helper_launched(),
            result == static_cast<int>(airshot::ExitCode::success),
            app.system_session_ending())) {
        std::wstring update_error;
        (void)airshot::launch_pending_update(
            false,
            app.automatic_updates_enabled(),
            &update_error);
    }
    return result;
}
