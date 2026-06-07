#include "host_app.h"
#include "cli_app.h"
#include "airshot/ocr.h"

#include "airshot/portable.h"

#include <shellapi.h>
#include <shellscalingapi.h>

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

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto arguments = command_arguments();
    if (!arguments.empty() && arguments[0] == L"--ocr-internal") {
        return airshot::run_ocr_cli(arguments);
    }
    if (!arguments.empty() &&
        (arguments[0] == L"--apply-update" || arguments[0] == L"--verify-update" ||
         arguments[0] == L"--check-update-target")) {
        return airshot::run_update_helper(arguments);
    }
    if (!arguments.empty() && arguments[0] != L"--transient") {
        return airshot::run_cli(arguments);
    }

    const bool transient = !arguments.empty() && arguments[0] == L"--transient";
    if (!transient) {
        std::wstring update_error;
        if (airshot::launch_pending_update(true, &update_error)) {
            return 0;
        }
        if (!update_error.empty()) {
            MessageBoxW(nullptr, update_error.c_str(), airshot::kAppName, MB_OK | MB_ICONERROR);
        }
        airshot::cleanup_stale_updates();
    }
    airshot::HostApp app(instance, transient);
    const int result = app.run();
    if (!transient) {
        std::wstring update_error;
        if (!airshot::launch_pending_update(false, &update_error) && !update_error.empty()) {
            MessageBoxW(nullptr, update_error.c_str(), airshot::kAppName, MB_OK | MB_ICONERROR);
        }
    }
    return result;
}
