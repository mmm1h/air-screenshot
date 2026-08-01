#include "airshot/ipc.h"

#include <bcrypt.h>
#include <sddl.h>

#include <array>
#include <atomic>
#include <limits>
#include <system_error>
#include <thread>
#include <utility>

#pragma comment(lib, "advapi32.lib")

namespace airshot {
namespace {

constexpr std::uint32_t kMaximumFrameSize = 1024U * 1024U;
constexpr DWORD kFrameTimeoutMs = 5000;
constexpr DWORD kPipeBufferSize = 64U * 1024U;
constexpr std::size_t kMaximumClientWorkers = 16;
constexpr unsigned int kListenerCreateAttempts = 20;
constexpr DWORD kListenerCreateRetryDelayMs = 50;

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_{};
};

class LocalAllocation {
public:
    LocalAllocation() = default;
    explicit LocalAllocation(void* value) noexcept : value_(value) {}
    ~LocalAllocation() {
        reset();
    }

    LocalAllocation(const LocalAllocation&) = delete;
    LocalAllocation& operator=(const LocalAllocation&) = delete;

    LocalAllocation(LocalAllocation&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    LocalAllocation& operator=(LocalAllocation&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] void* get() const noexcept {
        return value_;
    }

    void reset(void* value = nullptr) noexcept {
        if (value_) {
            LocalFree(value_);
        }
        value_ = value;
    }

private:
    void* value_{};
};

struct ProcessIdentity {
    std::wstring pipe_name;
    std::wstring sid_string;
};

[[nodiscard]] std::optional<ProcessIdentity> load_process_identity() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return std::nullopt;
    }
    UniqueHandle token(raw_token);

    DWORD token_user_size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &token_user_size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || token_user_size == 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> token_user_buffer(token_user_size);
    if (!GetTokenInformation(
            token.get(), TokenUser, token_user_buffer.data(), token_user_size, &token_user_size)) {
        return std::nullopt;
    }

    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(token_user_buffer.data());
    if (!IsValidSid(token_user->User.Sid)) {
        return std::nullopt;
    }

    LPWSTR raw_sid_string = nullptr;
    if (!ConvertSidToStringSidW(token_user->User.Sid, &raw_sid_string)) {
        return std::nullopt;
    }
    LocalAllocation sid_string_owner(raw_sid_string);

    DWORD session_id = 0;
    DWORD returned_size = 0;
    if (!GetTokenInformation(
            token.get(), TokenSessionId, &session_id, sizeof(session_id), &returned_size)) {
        return std::nullopt;
    }

    const auto* sid_bytes = static_cast<const std::uint8_t*>(token_user->User.Sid);
    const DWORD sid_size = GetLengthSid(token_user->User.Sid);
    std::uint64_t sid_digest = 14695981039346656037ULL;
    for (DWORD index = 0; index < sid_size; ++index) {
        sid_digest ^= sid_bytes[index];
        sid_digest *= 1099511628211ULL;
    }

    ProcessIdentity identity;
    identity.sid_string = raw_sid_string;
    identity.pipe_name =
        std::format(LR"(\\.\pipe\LOCAL\AirScreenshot.v2.s{}.u{:016X})", session_id, sid_digest);
    return identity;
}

[[nodiscard]] const std::optional<ProcessIdentity>& process_identity() {
    static const auto identity = load_process_identity();
    return identity;
}

class PipeSecurity {
public:
    PipeSecurity() {
        const auto& identity = process_identity();
        if (!identity) {
            return;
        }

        const std::wstring sddl =
            L"D:P(A;;GA;;;SY)(A;;GA;;;" + identity->sid_string + L")";
        PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &raw_descriptor, nullptr)) {
            return;
        }
        descriptor_.reset(raw_descriptor);
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = descriptor_.get();
        attributes_.bInheritHandle = FALSE;
        valid_ = true;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid_;
    }

    [[nodiscard]] SECURITY_ATTRIBUTES* get() noexcept {
        return valid_ ? &attributes_ : nullptr;
    }

private:
    LocalAllocation descriptor_;
    SECURITY_ATTRIBUTES attributes_{};
    bool valid_{};
};

class Deadline {
public:
    explicit Deadline(DWORD timeout_ms) noexcept
        : infinite_(timeout_ms == INFINITE),
          expires_at_(infinite_ ? 0 : GetTickCount64() + timeout_ms) {}

    [[nodiscard]] DWORD remaining() const noexcept {
        if (infinite_) {
            return INFINITE;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= expires_at_) {
            return 0;
        }
        return static_cast<DWORD>(
            std::min<ULONGLONG>(expires_at_ - now, std::numeric_limits<DWORD>::max() - 1ULL));
    }

private:
    bool infinite_{};
    ULONGLONG expires_at_{};
};

[[nodiscard]] DWORD minimum_timeout(DWORD first, DWORD second) noexcept {
    if (first == INFINITE) {
        return second;
    }
    if (second == INFINITE) {
        return first;
    }
    return std::min(first, second);
}

enum class IoResult {
    success,
    failed,
    cancelled,
    timed_out,
};

template <typename BeginOperation>
IoResult run_overlapped(HANDLE pipe,
                        HANDLE cancellation_event,
                        Deadline& deadline,
                        DWORD* transferred,
                        BeginOperation&& begin_operation) {
    if (cancellation_event &&
        WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0) {
        return IoResult::cancelled;
    }

    UniqueHandle operation_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!operation_event) {
        return IoResult::failed;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = operation_event.get();
    DWORD immediate_transfer = 0;
    if (begin_operation(&overlapped, &immediate_transfer)) {
        *transferred = immediate_transfer;
        return IoResult::success;
    }
    if (GetLastError() != ERROR_IO_PENDING) {
        return IoResult::failed;
    }

    std::array<HANDLE, 2> wait_handles{operation_event.get(), cancellation_event};
    const DWORD wait_count = cancellation_event ? 2U : 1U;
    const DWORD wait_result =
        WaitForMultipleObjects(wait_count, wait_handles.data(), FALSE, deadline.remaining());
    if (wait_result == WAIT_OBJECT_0) {
        return GetOverlappedResult(pipe, &overlapped, transferred, FALSE) ? IoResult::success
                                                                         : IoResult::failed;
    }

    CancelIoEx(pipe, &overlapped);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
    if (wait_result == WAIT_OBJECT_0 + 1U) {
        return IoResult::cancelled;
    }
    return wait_result == WAIT_TIMEOUT ? IoResult::timed_out : IoResult::failed;
}

IoResult read_exact(HANDLE pipe,
                    HANDLE cancellation_event,
                    void* buffer,
                    std::uint32_t size,
                    Deadline& deadline) {
    auto* cursor = static_cast<std::uint8_t*>(buffer);
    std::uint32_t remaining = size;
    while (remaining > 0) {
        DWORD transferred = 0;
        const IoResult result = run_overlapped(
            pipe,
            cancellation_event,
            deadline,
            &transferred,
            [&](OVERLAPPED* overlapped, DWORD* immediate_transfer) {
                if (!ReadFile(pipe, cursor, remaining, nullptr, overlapped)) {
                    return false;
                }
                return GetOverlappedResult(
                           pipe, overlapped, immediate_transfer, FALSE) != FALSE;
            });
        if (result != IoResult::success) {
            return result;
        }
        if (transferred == 0 || transferred > remaining) {
            return IoResult::failed;
        }
        cursor += transferred;
        remaining -= transferred;
    }
    return IoResult::success;
}

IoResult write_exact(HANDLE pipe,
                     HANDLE cancellation_event,
                     const void* buffer,
                     std::uint32_t size,
                     Deadline& deadline) {
    const auto* cursor = static_cast<const std::uint8_t*>(buffer);
    std::uint32_t remaining = size;
    while (remaining > 0) {
        DWORD transferred = 0;
        const IoResult result = run_overlapped(
            pipe,
            cancellation_event,
            deadline,
            &transferred,
            [&](OVERLAPPED* overlapped, DWORD* immediate_transfer) {
                if (!WriteFile(pipe, cursor, remaining, nullptr, overlapped)) {
                    return false;
                }
                return GetOverlappedResult(
                           pipe, overlapped, immediate_transfer, FALSE) != FALSE;
            });
        if (result != IoResult::success) {
            return result;
        }
        if (transferred == 0 || transferred > remaining) {
            return IoResult::failed;
        }
        cursor += transferred;
        remaining -= transferred;
    }
    return IoResult::success;
}

[[nodiscard]] bool write_frame(HANDLE pipe,
                               HANDLE cancellation_event,
                               std::string_view bytes,
                               Deadline& deadline) {
    if (bytes.size() > kMaximumFrameSize) {
        return false;
    }

    const auto size = static_cast<std::uint32_t>(bytes.size());
    if (write_exact(pipe, cancellation_event, &size, sizeof(size), deadline) != IoResult::success) {
        return false;
    }
    return bytes.empty() ||
           write_exact(pipe, cancellation_event, bytes.data(), size, deadline) == IoResult::success;
}

[[nodiscard]] bool write_frame(HANDLE pipe,
                               HANDLE cancellation_event,
                               std::string_view bytes,
                               DWORD timeout_ms) {
    Deadline deadline(timeout_ms);
    return write_frame(pipe, cancellation_event, bytes, deadline);
}

[[nodiscard]] std::optional<std::string> read_frame(HANDLE pipe,
                                                    HANDLE cancellation_event,
                                                    DWORD first_byte_timeout_ms) {
    std::uint32_t size = 0;
    Deadline first_byte_deadline(first_byte_timeout_ms);
    if (read_exact(pipe, cancellation_event, &size, 1, first_byte_deadline) != IoResult::success) {
        return std::nullopt;
    }

    Deadline frame_deadline(kFrameTimeoutMs);
    auto* size_bytes = reinterpret_cast<std::uint8_t*>(&size);
    if (read_exact(pipe,
                   cancellation_event,
                   size_bytes + 1,
                   static_cast<std::uint32_t>(sizeof(size) - 1),
                   frame_deadline) != IoResult::success ||
        size > kMaximumFrameSize) {
        return std::nullopt;
    }

    std::string bytes(size, '\0');
    if (size > 0 &&
        read_exact(pipe, cancellation_event, bytes.data(), size, frame_deadline) != IoResult::success) {
        return std::nullopt;
    }
    return bytes;
}

[[nodiscard]] std::optional<std::string> read_frame(HANDLE pipe,
                                                    HANDLE cancellation_event,
                                                    Deadline& request_deadline) {
    std::uint32_t size = 0;
    if (read_exact(pipe, cancellation_event, &size, 1, request_deadline) != IoResult::success) {
        return std::nullopt;
    }

    Deadline frame_deadline(minimum_timeout(kFrameTimeoutMs, request_deadline.remaining()));
    auto* size_bytes = reinterpret_cast<std::uint8_t*>(&size);
    if (read_exact(pipe,
                   cancellation_event,
                   size_bytes + 1,
                   static_cast<std::uint32_t>(sizeof(size) - 1),
                   frame_deadline) != IoResult::success ||
        size > kMaximumFrameSize) {
        return std::nullopt;
    }

    std::string bytes(size, '\0');
    if (size > 0 &&
        read_exact(pipe, cancellation_event, bytes.data(), size, frame_deadline) != IoResult::success) {
        return std::nullopt;
    }
    return bytes;
}

[[nodiscard]] std::optional<std::uint64_t> create_acknowledgement_token() {
    std::uint64_t token = 0;
    if (BCryptGenRandom(nullptr,
                        reinterpret_cast<PUCHAR>(&token),
                        static_cast<ULONG>(sizeof(token)),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return std::nullopt;
    }
    return token;
}

[[nodiscard]] bool write_acknowledgement_token(HANDLE pipe,
                                               HANDLE cancellation_event,
                                               std::uint64_t token,
                                               Deadline& deadline) {
    return write_exact(pipe,
                       cancellation_event,
                       &token,
                       static_cast<std::uint32_t>(sizeof(token)),
                       deadline) == IoResult::success;
}

[[nodiscard]] bool write_acknowledgement_token(HANDLE pipe,
                                               HANDLE cancellation_event,
                                               std::uint64_t token,
                                               DWORD timeout_ms) {
    Deadline deadline(timeout_ms);
    return write_acknowledgement_token(pipe, cancellation_event, token, deadline);
}

[[nodiscard]] std::optional<std::uint64_t> read_acknowledgement_token(
    HANDLE pipe,
    HANDLE cancellation_event,
    Deadline& deadline) {
    std::uint64_t token = 0;
    if (read_exact(pipe,
                   cancellation_event,
                   &token,
                   static_cast<std::uint32_t>(sizeof(token)),
                   deadline) != IoResult::success) {
        return std::nullopt;
    }
    return token;
}

[[nodiscard]] std::optional<std::uint64_t> read_acknowledgement_token(
    HANDLE pipe,
    HANDLE cancellation_event,
    DWORD timeout_ms) {
    Deadline deadline(timeout_ms);
    return read_acknowledgement_token(pipe, cancellation_event, deadline);
}

[[nodiscard]] UniqueHandle open_client_pipe(std::wstring_view pipe_name, DWORD timeout_ms) {
    Deadline deadline(timeout_ms);
    for (;;) {
        HANDLE pipe = CreateFileW(std::wstring(pipe_name).c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                  nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return UniqueHandle(pipe);
        }

        const DWORD error = GetLastError();
        const DWORD remaining = deadline.remaining();
        if (remaining == 0 || (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)) {
            return {};
        }

        if (error == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeW(std::wstring(pipe_name).c_str(), remaining) &&
                GetLastError() != ERROR_SEM_TIMEOUT) {
                return {};
            }
        } else {
            Sleep(std::min<DWORD>(remaining, 10));
        }
    }
}

[[nodiscard]] std::optional<std::wstring> exchange(HANDLE pipe,
                                                   std::wstring_view request,
                                                   Deadline& request_deadline) {
    const std::string request_bytes = to_utf8(request);
    if ((!request.empty() && request_bytes.empty()) || request_bytes.size() > kMaximumFrameSize) {
        return std::nullopt;
    }

    const DWORD write_timeout = minimum_timeout(kFrameTimeoutMs, request_deadline.remaining());
    Deadline write_deadline(write_timeout);
    if (!write_frame(pipe, nullptr, request_bytes, write_deadline)) {
        return std::nullopt;
    }

    const auto response_bytes = read_frame(pipe, nullptr, request_deadline);
    if (!response_bytes) {
        return std::nullopt;
    }

    Deadline acknowledgement_deadline(
        minimum_timeout(kFrameTimeoutMs, request_deadline.remaining()));
    const auto acknowledgement_token =
        read_acknowledgement_token(pipe, nullptr, acknowledgement_deadline);
    if (!acknowledgement_token ||
        !write_acknowledgement_token(
            pipe, nullptr, *acknowledgement_token, acknowledgement_deadline)) {
        return std::nullopt;
    }

    const std::wstring response = from_utf8(*response_bytes);
    if (!response_bytes->empty() && response.empty()) {
        return std::nullopt;
    }
    return response;
}

[[nodiscard]] UniqueHandle create_server_pipe(
    std::wstring_view pipe_name,
    bool first_instance,
    DWORD maximum_pipe_instances,
    DWORD* error = nullptr) {
    if (error) {
        *error = ERROR_SUCCESS;
    }
    const auto& identity = process_identity();
    if (!identity) {
        if (error) {
            *error = ERROR_NO_SUCH_LOGON_SESSION;
        }
        return {};
    }
    if (pipe_name.empty() || maximum_pipe_instances == 0 ||
        maximum_pipe_instances > PIPE_UNLIMITED_INSTANCES) {
        if (error) {
            *error = ERROR_INVALID_PARAMETER;
        }
        return {};
    }

    PipeSecurity security;
    if (!security) {
        if (error) {
            *error = ERROR_ACCESS_DENIED;
        }
        return {};
    }

    DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (first_instance) {
        open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    }
    HANDLE pipe = CreateNamedPipeW(std::wstring(pipe_name).c_str(),
                                   open_mode,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                       PIPE_REJECT_REMOTE_CLIENTS,
                                   maximum_pipe_instances,
                                   kPipeBufferSize,
                                   kPipeBufferSize,
                                   0,
                                   security.get());
    if (pipe == INVALID_HANDLE_VALUE && error) {
        *error = GetLastError();
    }
    return UniqueHandle(pipe == INVALID_HANDLE_VALUE ? nullptr : pipe);
}

[[nodiscard]] bool connect_server_pipe(HANDLE pipe, HANDLE cancellation_event) {
    Deadline deadline(INFINITE);
    DWORD transferred = 0;
    const IoResult result = run_overlapped(
        pipe,
        cancellation_event,
        deadline,
        &transferred,
        [&](OVERLAPPED* overlapped, DWORD*) {
            if (ConnectNamedPipe(pipe, overlapped)) {
                return true;
            }
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                SetEvent(overlapped->hEvent);
                return true;
            }
            SetLastError(error);
            return false;
        });
    return result == IoResult::success;
}

}  // namespace

bool pipe_is_available(DWORD timeout_ms) {
    const auto& identity = process_identity();
    return identity && pipe_is_available_at(identity->pipe_name, timeout_ms);
}

bool pipe_is_available_at(std::wstring_view pipe_name, DWORD timeout_ms) {
    if (pipe_name.empty()) {
        return false;
    }

    const std::wstring name(pipe_name);
    Deadline deadline(timeout_ms);
    bool first_attempt = true;
    for (;;) {
        const DWORD wait_timeout = deadline.remaining();
        if (!first_attempt && wait_timeout == 0) {
            return false;
        }
        first_attempt = false;
        if (WaitNamedPipeW(name.c_str(), wait_timeout) != FALSE) {
            return true;
        }

        const DWORD error = GetLastError();
        const DWORD remaining = deadline.remaining();
        if (remaining == 0 ||
            (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY &&
             error != ERROR_SEM_TIMEOUT)) {
            return false;
        }

        // WaitNamedPipe returns immediately when no pipe instance exists yet.
        // A host owns its mutex before it creates the listener, so poll within
        // the caller's deadline instead of reporting a false startup failure.
        Sleep(std::min<DWORD>(remaining, 10));
    }
}

std::optional<std::wstring> send_pipe_request(std::wstring_view request_json, DWORD timeout_ms) {
    const auto& identity = process_identity();
    if (!identity) {
        return std::nullopt;
    }
    return send_pipe_request_to(identity->pipe_name, request_json, timeout_ms);
}

std::optional<std::wstring> send_pipe_request_to(
    std::wstring_view pipe_name,
    std::wstring_view request_json,
    DWORD timeout_ms) {
    if (pipe_name.empty()) {
        return std::nullopt;
    }
    Deadline request_deadline(timeout_ms);
    const DWORD connection_timeout = minimum_timeout(kFrameTimeoutMs, request_deadline.remaining());
    UniqueHandle pipe = open_client_pipe(pipe_name, connection_timeout);
    if (!pipe) {
        return std::nullopt;
    }
    return exchange(pipe.get(), request_json, request_deadline);
}

struct PipeRequestContextAccess {
    [[nodiscard]] static PipeRequestContext create(
        std::uint64_t correlation_id, HANDLE pipe, HANDLE cancellation_event) noexcept {
        return PipeRequestContext(correlation_id, pipe, cancellation_event);
    }
};

bool PipeRequestContext::cancellation_requested() const noexcept {
    if (!pipe_ || pipe_ == INVALID_HANDLE_VALUE) {
        return true;
    }
    if (cancellation_event_ &&
        WaitForSingleObject(cancellation_event_, 0) == WAIT_OBJECT_0) {
        return true;
    }
    return PeekNamedPipe(pipe_, nullptr, 0, nullptr, nullptr, nullptr) == FALSE;
}

struct PipeServer::Impl {
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic_bool> finished;
    };

    Impl(std::wstring endpoint,
         DWORD instance_limit,
         CorrelatedHandler callback,
         CorrelatedResponseSentCallback sent_callback,
         ListenerFaultCallback fault_callback)
        : handler(std::move(callback)),
          response_sent_callback(std::move(sent_callback)),
          listener_fault_callback(std::move(fault_callback)),
          pipe_name(std::move(endpoint)),
          maximum_pipe_instances(instance_limit),
          accept_cancellation_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          accept_quiesced_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          io_cancellation_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~Impl() {
        stop();
    }

    [[nodiscard]] bool start() {
        if (!handler || !accept_cancellation_event || !accept_quiesced_event ||
            !io_cancellation_event) {
            return false;
        }
        UniqueHandle first_pipe =
            create_server_pipe(pipe_name, true, maximum_pipe_instances);
        if (!first_pipe) {
            return false;
        }
        try {
            workers.reserve(kMaximumClientWorkers);
            accept_thread = std::thread([this, pipe = std::move(first_pipe)]() mutable {
                run(std::move(pipe));
            });
        } catch (...) {
            return false;
        }
        return true;
    }

    void stop_accepting() {
        bool changed = false;
        {
            std::lock_guard lock(accept_state_mutex);
            changed = accepting.exchange(false, std::memory_order_acq_rel);
        }
        if (changed) {
            SetEvent(accept_cancellation_event.get());
        }
    }

    [[nodiscard]] bool wait_until_not_accepting(DWORD timeout_ms) const {
        return WaitForSingleObject(accept_quiesced_event.get(), timeout_ms) == WAIT_OBJECT_0;
    }

    [[nodiscard]] std::uint32_t active_clients() const noexcept {
        return active_client_count.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t allocate_correlation_id() noexcept {
        std::uint64_t id = next_correlation_id.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) {
            id = next_correlation_id.fetch_add(1, std::memory_order_relaxed);
        }
        return id;
    }

    void notify_response_sent(
        std::uint64_t correlation_id, std::wstring_view request, bool sent) noexcept {
        if (!response_sent_callback) {
            return;
        }
        try {
            response_sent_callback(correlation_id, request, sent);
        } catch (...) {
            // Completion notification failures do not affect pipe lifetime.
        }
    }

    void notify_listener_fault(DWORD error) noexcept {
        bool expected = true;
        if (!accepting.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel)) {
            return;
        }
        if (!listener_fault_callback) {
            return;
        }
        try {
            listener_fault_callback(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
        } catch (...) {
            // Listener health state remains authoritative if notification fails.
        }
    }

    [[nodiscard]] UniqueHandle create_replacement_pipe(DWORD* final_error) {
        DWORD error = ERROR_SUCCESS;
        for (unsigned int attempt = 0; attempt < kListenerCreateAttempts; ++attempt) {
            UniqueHandle replacement =
                create_server_pipe(pipe_name, false, maximum_pipe_instances, &error);
            if (replacement) {
                if (final_error) {
                    *final_error = ERROR_SUCCESS;
                }
                return replacement;
            }
            if (!accepting.load(std::memory_order_acquire) ||
                stopping.load(std::memory_order_acquire)) {
                break;
            }
            if (attempt + 1 < kListenerCreateAttempts &&
                WaitForSingleObject(
                    accept_cancellation_event.get(), kListenerCreateRetryDelayMs) ==
                    WAIT_OBJECT_0) {
                break;
            }
        }
        if (final_error) {
            *final_error = error;
        }
        return {};
    }

    void stop() {
        if (stopping.exchange(true)) {
            return;
        }
        stop_accepting();
        SetEvent(io_cancellation_event.get());
        if (accept_thread.joinable()) {
            accept_thread.join();
        }
    }

    void prune_finished_workers() {
        for (auto iterator = workers.begin(); iterator != workers.end();) {
            if (!iterator->finished->load(std::memory_order_acquire)) {
                ++iterator;
                continue;
            }
            if (iterator->thread.joinable()) {
                iterator->thread.join();
            }
            iterator = workers.erase(iterator);
        }
    }

    void join_workers() {
        for (Worker& worker : workers) {
            if (worker.thread.joinable()) {
                worker.thread.join();
            }
        }
        workers.clear();
    }

    void dispatch_client(UniqueHandle pipe) {
        prune_finished_workers();
        if (workers.size() >= kMaximumClientWorkers) {
            return;
        }

        active_client_count.fetch_add(1, std::memory_order_acq_rel);
        try {
            auto finished = std::make_shared<std::atomic_bool>(false);
            std::thread worker([this, client = std::move(pipe), finished]() mutable {
                serve_client(client.get());
                client.reset();
                active_client_count.fetch_sub(1, std::memory_order_acq_rel);
                finished->store(true, std::memory_order_release);
            });
            workers.push_back(Worker{std::move(worker), std::move(finished)});
        } catch (...) {
            active_client_count.fetch_sub(1, std::memory_order_acq_rel);
            // Refuse this client if the bounded worker cannot be created.
        }
    }

    void serve_client(HANDLE pipe) noexcept {
        try {
            const auto request_bytes = read_frame(pipe, io_cancellation_event.get(), kFrameTimeoutMs);
            if (!request_bytes || stopping.load(std::memory_order_acquire)) {
                return;
            }

            const std::wstring request = from_utf8(*request_bytes);
            if (!request_bytes->empty() && request.empty()) {
                return;
            }

            const std::uint64_t correlation_id = allocate_correlation_id();
            const PipeRequestContext context = PipeRequestContextAccess::create(
                correlation_id, pipe, io_cancellation_event.get());
            std::wstring response;
            try {
                response = handler(request, context);
            } catch (...) {
                notify_response_sent(correlation_id, request, false);
                return;
            }

            bool response_sent = false;
            try {
                const std::string response_bytes = to_utf8(response);
                if (!stopping.load(std::memory_order_acquire) &&
                    (response.empty() || !response_bytes.empty()) &&
                    response_bytes.size() <= kMaximumFrameSize) {
                    // Echoing a nonce proves the client consumed the complete response
                    // before the server flushes, disconnects, and reports completion.
                    const auto acknowledgement_token = create_acknowledgement_token();
                    if (acknowledgement_token &&
                        write_frame(
                            pipe, io_cancellation_event.get(), response_bytes, kFrameTimeoutMs) &&
                        write_acknowledgement_token(pipe,
                                                    io_cancellation_event.get(),
                                                    *acknowledgement_token,
                                                    kFrameTimeoutMs)) {
                        const auto echoed_token = read_acknowledgement_token(
                            pipe, io_cancellation_event.get(), kFrameTimeoutMs);
                        if (echoed_token && *echoed_token == *acknowledgement_token) {
                            const BOOL flushed = FlushFileBuffers(pipe);
                            const DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
                            const BOOL disconnected = DisconnectNamedPipe(pipe);
                            const DWORD disconnect_error =
                                disconnected ? ERROR_SUCCESS : GetLastError();
                            const bool peer_closed =
                                flush_error == ERROR_BROKEN_PIPE ||
                                flush_error == ERROR_NO_DATA ||
                                flush_error == ERROR_PIPE_NOT_CONNECTED;
                            const bool already_disconnected =
                                disconnect_error == ERROR_BROKEN_PIPE ||
                                disconnect_error == ERROR_NO_DATA ||
                                disconnect_error == ERROR_PIPE_NOT_CONNECTED;
                            response_sent = (flushed || peer_closed) &&
                                            (disconnected || already_disconnected);
                        }
                    }
                }
            } catch (...) {
                // A completed handler still receives a failed transport notification.
            }

            notify_response_sent(correlation_id, request, response_sent);
        } catch (...) {
            // A malformed request or handler failure terminates only this client.
        }
    }

    void run(UniqueHandle listener) {
        while (accepting.load(std::memory_order_acquire) &&
               !stopping.load(std::memory_order_acquire)) {
            if (!connect_server_pipe(listener.get(), accept_cancellation_event.get())) {
                if (!accepting.load(std::memory_order_acquire) ||
                    stopping.load(std::memory_order_acquire)) {
                    break;
                }
                listener.reset();
                DWORD error = ERROR_SUCCESS;
                listener = create_replacement_pipe(&error);
                if (!listener) {
                    if (accepting.load(std::memory_order_acquire) &&
                        !stopping.load(std::memory_order_acquire)) {
                        notify_listener_fault(error);
                    }
                    break;
                }
                continue;
            }
            UniqueHandle client;
            {
                std::lock_guard lock(accept_state_mutex);
                if (!accepting.load(std::memory_order_acquire) ||
                    stopping.load(std::memory_order_acquire)) {
                    break;
                }
                client.reset(listener.release());
            }
            dispatch_client(std::move(client));
            DWORD error = ERROR_SUCCESS;
            listener = create_replacement_pipe(&error);
            if (!listener) {
                if (accepting.load(std::memory_order_acquire) &&
                    !stopping.load(std::memory_order_acquire)) {
                    notify_listener_fault(error);
                }
                break;
            }
        }
        listener.reset();
        SetEvent(accept_quiesced_event.get());
        join_workers();
    }

    CorrelatedHandler handler;
    CorrelatedResponseSentCallback response_sent_callback;
    ListenerFaultCallback listener_fault_callback;
    std::wstring pipe_name;
    DWORD maximum_pipe_instances{PIPE_UNLIMITED_INSTANCES};
    std::atomic_bool accepting{true};
    std::atomic_bool stopping{false};
    std::atomic_uint32_t active_client_count{0};
    std::atomic_uint64_t next_correlation_id{1};
    std::mutex accept_state_mutex;
    UniqueHandle accept_cancellation_event;
    UniqueHandle accept_quiesced_event;
    UniqueHandle io_cancellation_event;
    std::thread accept_thread;
    std::vector<Worker> workers;
};

PipeServer::PipeServer() = default;

PipeServer::PipeServer(std::wstring pipe_name)
    : pipe_name_(std::move(pipe_name)) {}

PipeServer::PipeServer(std::wstring pipe_name, DWORD maximum_pipe_instances)
    : pipe_name_(std::move(pipe_name)),
      maximum_pipe_instances_(maximum_pipe_instances) {}

PipeServer::~PipeServer() {
    stop();
}

bool PipeServer::start(Handler handler) {
    return start(std::move(handler), {});
}

bool PipeServer::start(Handler handler, ResponseSentCallback response_sent_callback) {
    if (!handler) {
        return false;
    }
    CorrelatedHandler correlated_handler =
        [handler = std::move(handler)](
            std::wstring_view request, const PipeRequestContext&) {
            return handler(request);
        };
    CorrelatedResponseSentCallback correlated_callback;
    if (response_sent_callback) {
        correlated_callback =
            [callback = std::move(response_sent_callback)](
                std::uint64_t, std::wstring_view request, bool sent) {
                callback(request, sent);
            };
    }
    return start_correlated(std::move(correlated_handler), std::move(correlated_callback));
}

bool PipeServer::start_correlated(
    CorrelatedHandler handler,
    CorrelatedResponseSentCallback response_sent_callback,
    ListenerFaultCallback listener_fault_callback) {
    std::lock_guard operation_lock(operation_mutex_);
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (impl_) {
            return false;
        }
    }
    std::wstring endpoint = pipe_name_;
    if (endpoint.empty()) {
        const auto& identity = process_identity();
        if (!identity) {
            return false;
        }
        endpoint = identity->pipe_name;
    }
    auto implementation = std::make_shared<Impl>(
        std::move(endpoint),
        maximum_pipe_instances_,
        std::move(handler),
        std::move(response_sent_callback),
        std::move(listener_fault_callback));
    if (!implementation->start()) {
        return false;
    }
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        impl_ = std::move(implementation);
    }
    return true;
}

void PipeServer::stop_accepting() {
    std::shared_ptr<Impl> implementation;
    {
        std::lock_guard lock(lifecycle_mutex_);
        implementation = impl_;
    }
    if (implementation) {
        implementation->stop_accepting();
    }
}

bool PipeServer::wait_until_not_accepting(DWORD timeout_ms) {
    std::shared_ptr<Impl> implementation;
    {
        std::lock_guard lock(lifecycle_mutex_);
        implementation = impl_;
    }
    return !implementation || implementation->wait_until_not_accepting(timeout_ms);
}

std::uint32_t PipeServer::active_clients() {
    std::shared_ptr<Impl> implementation;
    {
        std::lock_guard lock(lifecycle_mutex_);
        implementation = impl_;
    }
    return implementation ? implementation->active_clients() : 0;
}

void PipeServer::stop() {
    std::lock_guard operation_lock(operation_mutex_);
    std::shared_ptr<Impl> implementation;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        implementation = impl_;
    }
    if (!implementation) {
        return;
    }
    implementation->stop();
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (impl_ == implementation) {
            impl_.reset();
        }
    }
}

}  // namespace airshot
