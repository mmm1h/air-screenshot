#include "cli_app.h"

#include "airshot/command.h"
#include "airshot/ipc.h"
#include "airshot/portable.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <limits>

namespace airshot {
namespace {

void write_console(HANDLE handle, std::wstring_view text) noexcept {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            text.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteConsoleW(handle, text.data() + offset, requested, &written, nullptr) ||
            written == 0) {
            return;
        }
        offset += written;
    }
}

void write_bytes(HANDLE handle, std::string_view bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, requested, &written, nullptr) ||
            written == 0) {
            return;
        }
        offset += written;
    }
}

void write_stream(std::wstring_view text, bool error = false) {
    std::wstring line(text);
    if (line.empty() || line.back() != L'\n') {
        line.push_back(L'\n');
    }
    HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (handle && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        write_console(handle, line);
        return;
    }
    const std::string utf8 = to_utf8(line);
    if (handle && handle != INVALID_HANDLE_VALUE) {
        write_bytes(handle, utf8);
    }
}

std::wstring create_launch_nonce() {
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(
            nullptr,
            random.data(),
            static_cast<ULONG>(random.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }

    constexpr std::wstring_view hex = L"0123456789abcdef";
    std::wstring nonce(random.size() * 2, L'0');
    for (std::size_t index = 0; index < random.size(); ++index) {
        nonce[index * 2] = hex[random[index] >> 4U];
        nonce[index * 2 + 1] = hex[random[index] & 0x0fU];
    }
    return nonce;
}

bool host_instance_exists() noexcept;

bool launch_host(bool transient, std::wstring_view launch_nonce) {
    const std::filesystem::path host = portable_executable_path();
    std::wstring command = std::format(L"\"{}\"", host.wstring());
    if (transient) {
        command.append(L" --transient=");
        command.append(launch_nonce);
    }
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(host.c_str(),
                                        command.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        host.parent_path().c_str(),
                                        &startup,
                                        &process);
    if (!created) {
        return false;
    }
    CloseHandle(process.hThread);
    const ULONGLONG deadline = GetTickCount64() + 15000;
    bool initial_process_exited = false;
    for (;;) {
        if (pipe_is_available(100)) {
            CloseHandle(process.hProcess);
            return true;
        }
        if (!initial_process_exited &&
            WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            initial_process_exited = true;
        }
        if (initial_process_exited && !host_instance_exists()) {
            CloseHandle(process.hProcess);
            return false;
        }
        if (GetTickCount64() >= deadline) {
            CloseHandle(process.hProcess);
            return false;
        }
        Sleep(25);
    }
}

void write_response(const CommandResponse& response, bool json) {
    if (json) {
        write_stream(response_to_json(response));
        return;
    }

    const bool error = response.code != ExitCode::success;
    if (!response.text.empty()) {
        write_stream(response.text, error);
    }
    if (!response.path.empty()) {
        write_stream(response.path, error);
    }
    if (!response.message.empty()) {
        write_stream(response.message, error);
    }
    if (!response.data_json.empty()) {
        write_stream(response.data_json, error);
    }
}

CommandResponse ipc_failure(std::wstring message) {
    CommandResponse response;
    response.code = ExitCode::ipc_failed;
    response.message = std::move(message);
    response.error_type = L"ipc_failed";
    return response;
}

bool host_instance_exists() noexcept {
    HANDLE mutex = OpenMutexW(
        SYNCHRONIZE | MUTEX_MODIFY_STATE,
        FALSE,
        kHostMutexName);
    if (!mutex) {
        return false;
    }
    const DWORD state = WaitForSingleObject(mutex, 0);
    const bool held_by_host = state == WAIT_TIMEOUT;
    if (state == WAIT_OBJECT_0 || state == WAIT_ABANDONED) {
        ReleaseMutex(mutex);
    }
    CloseHandle(mutex);
    return held_by_host;
}

enum class HostPipeWaitResult {
    ready,
    host_gone,
    timed_out,
};

HostPipeWaitResult wait_for_host_pipe(DWORD timeout_ms) noexcept {
    const ULONGLONG started_at = GetTickCount64();
    for (;;) {
        if (pipe_is_available(0)) {
            return HostPipeWaitResult::ready;
        }
        if (!host_instance_exists()) {
            return HostPipeWaitResult::host_gone;
        }

        const ULONGLONG elapsed = GetTickCount64() - started_at;
        if (elapsed >= timeout_ms) {
            return HostPipeWaitResult::timed_out;
        }
        Sleep(std::min<DWORD>(
            10,
            timeout_ms - static_cast<DWORD>(elapsed)));
    }
}

}  // namespace

int run_cli(std::span<const std::wstring> arguments) {
    const auto parsed = parse_cli(arguments);
    if (parsed.local_only) {
        write_stream(parsed.local_text, !parsed.json && parsed.code != ExitCode::success);
        return static_cast<int>(parsed.code);
    }

    const auto* app = parsed.command ? std::get_if<AppCommand>(&*parsed.command) : nullptr;
    const bool app_status = app && app->action == AppAction::status;
    const bool app_start = app && app->action == AppAction::start;
    const bool app_stop = app && app->action == AppAction::stop;

    std::wstring request_json = parsed.request_json;
    bool pipe_available = pipe_is_available(0);
    if (!pipe_available && host_instance_exists()) {
        const HostPipeWaitResult wait_result = wait_for_host_pipe(15000);
        pipe_available = wait_result == HostPipeWaitResult::ready;
        if (wait_result == HostPipeWaitResult::timed_out) {
            const CommandResponse response =
                ipc_failure(L"Air Screenshot 宿主存在，但命名管道未就绪。");
            write_response(response, parsed.json);
            return static_cast<int>(response.code);
        }
        if (wait_result == HostPipeWaitResult::host_gone) {
            // A replacement host may already be ready. If it is still
            // starting, the normal launch/status path below remains safe.
            pipe_available = pipe_is_available(0);
        }
    }
    if (!pipe_available) {
        if (app_status || app_stop) {
            CommandResponse response;
            response.message = L"Air Screenshot 未运行。";
            if (parsed.json) {
                response.data_json = LR"({"running":false,"transient":false,"shell":false})";
            }
            write_response(response, parsed.json);
            return static_cast<int>(response.code);
        }
        // Settings launches are transient until the host sees whether shell
        // integration is enabled. This prevents cancelling settings with
        // shell=false from leaving an invisible persistent process.
        const bool persistent = app_start;
        std::wstring launch_nonce;
        if (!persistent) {
            launch_nonce = create_launch_nonce();
            if (launch_nonce.empty() || !parsed.command) {
                const CommandResponse response =
                    ipc_failure(L"无法创建 Air Screenshot 临时宿主启动标识。");
                write_response(response, parsed.json);
                return static_cast<int>(response.code);
            }
            request_json = command_to_json(*parsed.command, parsed.json, launch_nonce);
            if (request_json.empty()) {
                const CommandResponse response =
                    ipc_failure(L"无法创建 Air Screenshot 宿主请求。");
                write_response(response, parsed.json);
                return static_cast<int>(response.code);
            }
        }
        if (!launch_host(!persistent, launch_nonce)) {
            const CommandResponse response = ipc_failure(L"无法启动 Air Screenshot 宿主。");
            write_response(response, parsed.json);
            return static_cast<int>(response.code);
        }
    }

    const auto raw_response = send_pipe_request(request_json);
    if (!raw_response) {
        const CommandResponse response = ipc_failure(L"无法连接 Air Screenshot 宿主。");
        write_response(response, parsed.json);
        return static_cast<int>(response.code);
    }
    const auto response = response_from_json(*raw_response);
    write_response(response, parsed.json);
    return static_cast<int>(response.code);
}

}  // namespace airshot
