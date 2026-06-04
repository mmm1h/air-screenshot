#include "airshot/command.h"
#include "airshot/ipc.h"

namespace {

void write_stream(std::wstring_view text, bool error = false) {
    std::wstring line(text);
    if (line.empty() || line.back() != L'\n') {
        line.push_back(L'\n');
    }
    HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        DWORD written = 0;
        WriteConsoleW(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        return;
    }
    const std::string utf8 = airshot::to_utf8(line);
    DWORD written = 0;
    if (handle != INVALID_HANDLE_VALUE) {
        WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

std::filesystem::path executable_directory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool launch_host(bool transient) {
    const std::filesystem::path host = executable_directory() / L"AirScreenshot.exe";
    std::wstring command = std::format(L"\"{}\"{}", host.wstring(), transient ? L" --transient" : L"");
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
    CloseHandle(process.hProcess);
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (airshot::pipe_is_available(50)) {
            return true;
        }
        Sleep(20);
    }
    return false;
}

bool argument_is(std::span<const std::wstring> arguments, std::size_t index, std::wstring_view value) {
    return index < arguments.size() && _wcsicmp(arguments[index].c_str(), std::wstring(value).c_str()) == 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    const auto parsed = airshot::parse_cli(arguments);
    if (parsed.local_only) {
        write_stream(parsed.local_text, parsed.code != airshot::ExitCode::success);
        return static_cast<int>(parsed.code);
    }

    const bool app_command = argument_is(arguments, 0, L"app");
    const bool app_status = app_command && argument_is(arguments, 1, L"status");
    const bool app_start = app_command && argument_is(arguments, 1, L"start");
    const bool app_stop = app_command && argument_is(arguments, 1, L"stop");
    const bool app_settings = app_command && argument_is(arguments, 1, L"settings");

    if (!airshot::pipe_is_available(0)) {
        if (app_status || app_stop) {
            write_stream(L"Air Screenshot 未运行。");
            return 0;
        }
        const bool persistent = app_start || app_settings;
        if (!launch_host(!persistent)) {
            write_stream(L"无法启动 Air Screenshot 宿主。", true);
            return static_cast<int>(airshot::ExitCode::ipc_failed);
        }
    }

    const auto raw_response = airshot::send_pipe_request(parsed.request_json);
    if (!raw_response) {
        write_stream(L"无法连接 Air Screenshot 宿主。", true);
        return static_cast<int>(airshot::ExitCode::ipc_failed);
    }
    const auto response = airshot::response_from_json(*raw_response);
    if (parsed.json) {
        write_stream(*raw_response);
    } else {
        const bool error = response.code != airshot::ExitCode::success;
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
    return static_cast<int>(response.code);
}
