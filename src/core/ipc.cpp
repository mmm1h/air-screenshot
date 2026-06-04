#include "airshot/ipc.h"

namespace airshot {
namespace {

bool read_exact(HANDLE pipe, void* buffer, DWORD size) {
    auto* cursor = static_cast<std::uint8_t*>(buffer);
    DWORD remaining = size;
    while (remaining > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, remaining, &read, nullptr) || read == 0) {
            return false;
        }
        cursor += read;
        remaining -= read;
    }
    return true;
}

bool write_exact(HANDLE pipe, const void* buffer, DWORD size) {
    const auto* cursor = static_cast<const std::uint8_t*>(buffer);
    DWORD remaining = size;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, remaining, &written, nullptr) || written == 0) {
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

std::optional<std::wstring> exchange(HANDLE pipe, std::wstring_view request) {
    const std::string bytes = to_utf8(request);
    if (bytes.size() > 1024U * 1024U) {
        return std::nullopt;
    }
    const auto size = static_cast<std::uint32_t>(bytes.size());
    if (!write_exact(pipe, &size, sizeof(size)) ||
        !write_exact(pipe, bytes.data(), static_cast<DWORD>(bytes.size()))) {
        return std::nullopt;
    }
    std::uint32_t response_size = 0;
    if (!read_exact(pipe, &response_size, sizeof(response_size)) || response_size > 1024U * 1024U) {
        return std::nullopt;
    }
    std::string response(response_size, '\0');
    if (!read_exact(pipe, response.data(), response_size)) {
        return std::nullopt;
    }
    return from_utf8(response);
}

}  // namespace

bool pipe_is_available(DWORD timeout_ms) {
    if (WaitNamedPipeW(kPipeName, timeout_ms)) {
        return true;
    }
    return GetLastError() == ERROR_SEM_TIMEOUT ? false : false;
}

std::optional<std::wstring> send_pipe_request(std::wstring_view request_json, DWORD timeout_ms) {
    if (!WaitNamedPipeW(kPipeName, timeout_ms)) {
        return std::nullopt;
    }
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    auto response = exchange(pipe, request_json);
    CloseHandle(pipe);
    return response;
}

PipeServer::~PipeServer() {
    stop();
}

bool PipeServer::start(Handler handler) {
    if (thread_.joinable()) {
        return false;
    }
    handler_ = std::move(handler);
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void PipeServer::stop() {
    stopping_ = true;
    if (thread_.joinable()) {
        HANDLE wake = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) {
            CloseHandle(wake);
        }
        CancelSynchronousIo(thread_.native_handle());
        thread_.join();
    }
}

void PipeServer::run() {
    while (!stopping_) {
        HANDLE pipe = CreateNamedPipeW(kPipeName,
                                       PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       1,
                                       1024U * 1024U,
                                       1024U * 1024U,
                                       0,
                                       nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            return;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected || stopping_) {
            CloseHandle(pipe);
            continue;
        }

        std::uint32_t request_size = 0;
        if (read_exact(pipe, &request_size, sizeof(request_size)) && request_size <= 1024U * 1024U) {
            std::string request(request_size, '\0');
            if (read_exact(pipe, request.data(), request_size)) {
                const std::wstring response_text = handler_(from_utf8(request));
                const std::string response = to_utf8(response_text);
                const auto response_size = static_cast<std::uint32_t>(response.size());
                write_exact(pipe, &response_size, sizeof(response_size));
                write_exact(pipe, response.data(), response_size);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

}  // namespace airshot
