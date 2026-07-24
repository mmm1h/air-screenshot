#pragma once

#include "airshot/common.h"

#include <functional>
#include <memory>
#include <mutex>

namespace airshot {

[[nodiscard]] bool pipe_is_available(DWORD timeout_ms = 0);
[[nodiscard]] bool pipe_is_available_at(
    std::wstring_view pipe_name,
    DWORD timeout_ms = 0);
[[nodiscard]] std::optional<std::wstring> send_pipe_request(
    std::wstring_view request_json,
    DWORD timeout_ms = 120000);
[[nodiscard]] std::optional<std::wstring> send_pipe_request_to(
    std::wstring_view pipe_name,
    std::wstring_view request_json,
    DWORD timeout_ms = 120000);

struct PipeRequestContextAccess;

class PipeRequestContext {
public:
    [[nodiscard]] std::uint64_t correlation_id() const noexcept {
        return correlation_id_;
    }
    // True when the server is stopping or the requesting client has closed its
    // pipe. The context is valid only while the handler is running.
    [[nodiscard]] bool cancellation_requested() const noexcept;

private:
    PipeRequestContext(
        std::uint64_t correlation_id, HANDLE pipe, HANDLE cancellation_event) noexcept
        : correlation_id_(correlation_id),
          pipe_(pipe),
          cancellation_event_(cancellation_event) {}

    std::uint64_t correlation_id_{};
    HANDLE pipe_{};
    HANDLE cancellation_event_{};

    friend struct PipeRequestContextAccess;
};

class PipeServer {
public:
    // Multiple clients may invoke these callbacks concurrently. request and
    // PipeRequestContext are valid only for the duration of the callback.
    // Correlated requests receive a transport-assigned, nonzero unique id.
    using Handler = std::function<std::wstring(std::wstring_view)>;
    using ResponseSentCallback = std::function<void(std::wstring_view request, bool sent)>;
    using CorrelatedHandler =
        std::function<std::wstring(std::wstring_view, const PipeRequestContext&)>;
    using CorrelatedResponseSentCallback =
        std::function<void(std::uint64_t correlation_id, std::wstring_view request, bool sent)>;
    // Runs at most once on the accept thread if the listener cannot be rebuilt
    // after bounded retries. Already accepted clients continue to drain.
    using ListenerFaultCallback = std::function<void(DWORD error)>;

    PipeServer();
    explicit PipeServer(std::wstring pipe_name);
    PipeServer(std::wstring pipe_name, DWORD maximum_pipe_instances);
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    bool start(Handler handler);
    bool start(Handler handler, ResponseSentCallback response_sent_callback);
    bool start_correlated(
        CorrelatedHandler handler,
        CorrelatedResponseSentCallback response_sent_callback,
        ListenerFaultCallback listener_fault_callback = {});
    // Stops accepting new clients without cancelling handlers or transports that
    // were already accepted. This call is non-blocking.
    void stop_accepting();
    [[nodiscard]] bool wait_until_not_accepting(DWORD timeout_ms);
    [[nodiscard]] std::uint32_t active_clients();
    // The owner must cancel work dispatched by handler before destruction.
    // stop() cancels all pending pipe I/O, then waits for active handlers to return.
    // It must not be called synchronously from either callback.
    void stop();

private:
    struct Impl;
    std::mutex operation_mutex_;
    std::mutex lifecycle_mutex_;
    std::wstring pipe_name_;
    DWORD maximum_pipe_instances_{PIPE_UNLIMITED_INSTANCES};
    std::shared_ptr<Impl> impl_;
};

}  // namespace airshot
