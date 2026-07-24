#include "airshot/ipc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

int failures = 0;

const std::wstring& test_pipe_name() {
    static const std::wstring value = std::format(
        LR"(\\.\pipe\LOCAL\AirScreenshot.Tests.{})",
        GetCurrentProcessId());
    return value;
}

bool pipe_is_available(DWORD timeout_ms = 0) {
    return airshot::pipe_is_available_at(test_pipe_name(), timeout_ms);
}

std::optional<std::wstring> send_pipe_request(
    std::wstring_view request,
    DWORD timeout_ms = 120000) {
    return airshot::send_pipe_request_to(
        test_pipe_name(), request, timeout_ms);
}

void expect(bool condition, std::wstring_view message) {
    if (!condition) {
        ++failures;
        std::wcerr << L"FAIL: " << message << L'\n';
    }
}

void test_round_trip_and_single_server() {
    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    std::vector<std::pair<std::wstring, bool>> callbacks;
    std::atomic_int active_handlers = 0;
    std::atomic_int maximum_active_handlers = 0;

    airshot::PipeServer server(test_pipe_name());
    expect(server.start(
               [&](std::wstring_view request) {
                   const int active = active_handlers.fetch_add(1) + 1;
                   int observed = maximum_active_handlers.load();
                   while (observed < active &&
                          !maximum_active_handlers.compare_exchange_weak(observed, active)) {
                   }
                   std::this_thread::sleep_for(std::chrono::milliseconds(20));
                   active_handlers.fetch_sub(1);
                   return L"reply:" + std::wstring(request);
               },
               [&](std::wstring_view request, bool sent) {
                   {
                       std::lock_guard lock(callback_mutex);
                       callbacks.emplace_back(request, sent);
                   }
                   callback_condition.notify_all();
               }),
           L"pipe server starts");
    expect(pipe_is_available(100), L"pipe is available to the current user and session");

    airshot::PipeServer competing_server(test_pipe_name());
    expect(!competing_server.start([](std::wstring_view) { return std::wstring{}; }),
           L"first-pipe-instance guard rejects a competing server");

    const auto response = send_pipe_request(L"ping", 2000);
    expect(response && *response == L"reply:ping", L"request and response round trip");

    constexpr int client_count = 12;
    std::vector<std::optional<std::wstring>> responses(client_count);
    std::vector<std::thread> clients;
    clients.reserve(client_count);
    for (int index = 0; index < client_count; ++index) {
        clients.emplace_back([&, index] {
            responses[index] =
                send_pipe_request(L"client-" + std::to_wstring(index), 5000);
        });
    }
    for (std::thread& client : clients) {
        client.join();
    }
    for (int index = 0; index < client_count; ++index) {
        expect(responses[index] &&
                   *responses[index] == L"reply:client-" + std::to_wstring(index),
               L"concurrent client receives its response");
    }
    expect(maximum_active_handlers.load() > 1,
           L"concurrent clients reach the host handler without transport-layer head-of-line blocking");

    {
        std::unique_lock lock(callback_mutex);
        callback_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return callbacks.size() == static_cast<std::size_t>(client_count + 1);
        });
    }
    expect(callbacks.size() == static_cast<std::size_t>(client_count + 1),
           L"response completion callback runs once per handled request");
    bool all_sent = true;
    for (const auto& [request, sent] : callbacks) {
        all_sent = all_sent && !request.empty() && sent;
    }
    expect(all_sent, L"response callback reports flushed and disconnected responses");

    const std::wstring oversized_request(1024U * 1024U + 1U, L'x');
    expect(!send_pipe_request(oversized_request, 100),
           L"oversized request is rejected before transmission");

    const auto stop_started = std::chrono::steady_clock::now();
    server.stop();
    expect(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(1),
           L"stop cancels a pending accept without a wake-up client");
    expect(!pipe_is_available(0), L"pipe is unavailable after stop");
}

void test_restart_and_legacy_start_overload() {
    airshot::PipeServer server(test_pipe_name());
    expect(server.start([](std::wstring_view request) { return std::wstring(request); }),
           L"legacy start overload remains supported");
    const auto response = send_pipe_request(L"restart", 2000);
    expect(response && *response == L"restart", L"legacy start overload serves requests");
    server.stop();

    expect(server.start([](std::wstring_view) { return std::wstring(L"again"); }),
           L"stopped server can be restarted");
    const auto restarted_response = send_pipe_request(L"request", 2000);
    expect(restarted_response && *restarted_response == L"again",
           L"restarted server serves requests");
    server.stop();
}

void test_stop_cancels_transport_after_handler_cancellation() {
    std::mutex handler_mutex;
    std::condition_variable handler_condition;
    bool handler_entered = false;
    bool handler_cancelled = false;
    std::atomic_bool callback_called = false;
    std::atomic_bool callback_sent = true;

    airshot::PipeServer server(test_pipe_name());
    expect(server.start(
               [&](std::wstring_view) {
                   std::unique_lock lock(handler_mutex);
                   handler_entered = true;
                   handler_condition.notify_all();
                   handler_condition.wait(lock, [&] { return handler_cancelled; });
                   return std::wstring(L"cancelled");
               },
               [&](std::wstring_view request, bool sent) {
                   callback_called = request == L"blocking";
                   callback_sent = sent;
               }),
           L"cancellation test server starts");

    std::optional<std::wstring> response;
    std::thread client([&] {
        response = send_pipe_request(L"blocking", 5000);
    });
    {
        std::unique_lock lock(handler_mutex);
        handler_condition.wait_for(lock, std::chrono::seconds(2), [&] { return handler_entered; });
    }
    expect(handler_entered, L"blocking handler receives request");

    const auto stop_started = std::chrono::steady_clock::now();
    std::thread stopper([&] { server.stop(); });
    expect(server.wait_until_not_accepting(2000),
           L"stop closes the listener before the handler is released");
    {
        std::lock_guard lock(handler_mutex);
        handler_cancelled = true;
    }
    handler_condition.notify_all();
    stopper.join();
    client.join();

    expect(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(1),
           L"stop completes after the owner releases its synchronous handler");
    expect(!response, L"stopping server cancels the pending response transport");
    expect(callback_called && !callback_sent,
           L"completion callback reports a response cancelled during stop");
}

void test_stop_accepting_drains_existing_client() {
    std::mutex handler_mutex;
    std::condition_variable handler_condition;
    bool handler_entered = false;
    bool release_handler = false;
    std::atomic_bool callback_called = false;
    std::atomic_bool callback_sent = false;

    airshot::PipeServer server(test_pipe_name());
    expect(server.start(
               [&](std::wstring_view request) {
                   std::unique_lock lock(handler_mutex);
                   handler_entered = request == L"drain";
                   handler_condition.notify_all();
                   handler_condition.wait(lock, [&] { return release_handler; });
                   return std::wstring(L"drained");
               },
               [&](std::wstring_view request, bool sent) {
                   callback_called = request == L"drain";
                   callback_sent = sent;
               }),
           L"graceful-drain test server starts");

    std::optional<std::wstring> response;
    std::thread client([&] {
        response = send_pipe_request(L"drain", 5000);
    });
    {
        std::unique_lock lock(handler_mutex);
        handler_condition.wait_for(lock, std::chrono::seconds(2), [&] { return handler_entered; });
    }
    expect(handler_entered, L"graceful-drain handler receives request");

    const auto stop_started = std::chrono::steady_clock::now();
    server.stop_accepting();
    expect(std::chrono::steady_clock::now() - stop_started < std::chrono::milliseconds(200),
           L"stop_accepting is non-blocking while a handler is active");
    expect(server.wait_until_not_accepting(2000),
           L"listener reports quiescence independently of active clients");
    expect(server.active_clients() == 1,
           L"accepted client remains visible while graceful drain is in progress");

    const auto unavailable_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pipe_is_available(0) && std::chrono::steady_clock::now() < unavailable_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(!pipe_is_available(0), L"stop_accepting closes the listener");

    {
        std::lock_guard lock(handler_mutex);
        release_handler = true;
    }
    handler_condition.notify_all();
    client.join();

    expect(response && *response == L"drained",
           L"client accepted before stop_accepting still receives its response");
    const auto drained_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.active_clients() != 0 && std::chrono::steady_clock::now() < drained_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(server.active_clients() == 0, L"active client count reaches zero after transport completion");
    expect(callback_called && callback_sent,
           L"drained response completes its acknowledgement handshake");
    server.stop();
}

void test_correlated_completion_and_handler_exception() {
    std::mutex order_mutex;
    std::condition_variable order_condition;
    std::vector<std::uint64_t> handler_ids;
    std::vector<std::uint64_t> callback_ids;
    std::uint64_t second_handler_id = 0;
    bool second_response_completed = false;
    bool all_correlated_sent = true;

    airshot::PipeServer correlated_server(test_pipe_name());
    expect(correlated_server.start_correlated(
               [&](std::wstring_view request, const airshot::PipeRequestContext& context) {
                   std::unique_lock lock(order_mutex);
                   const std::size_t ordinal = handler_ids.size();
                   handler_ids.push_back(context.correlation_id());
                   if (ordinal == 0) {
                       order_condition.wait_for(lock, std::chrono::seconds(2), [&] {
                           return second_response_completed;
                       });
                   } else {
                       second_handler_id = context.correlation_id();
                   }
                   lock.unlock();
                   return L"correlated:" + std::wstring(request) + L":" +
                          std::to_wstring(context.correlation_id());
               },
               [&](std::uint64_t id, std::wstring_view request, bool sent) {
                   {
                       std::lock_guard lock(order_mutex);
                       if (request == L"identical") {
                           callback_ids.push_back(id);
                           all_correlated_sent = all_correlated_sent && sent;
                           if (id == second_handler_id) {
                               second_response_completed = true;
                           }
                       }
                   }
                   order_condition.notify_all();
               }),
           L"correlated server starts");

    std::optional<std::wstring> first_response;
    std::optional<std::wstring> second_response;
    std::thread first_client([&] {
        first_response = send_pipe_request(L"identical", 5000);
    });
    std::thread second_client([&] {
        second_response = send_pipe_request(L"identical", 5000);
    });
    first_client.join();
    second_client.join();
    {
        std::unique_lock lock(order_mutex);
        order_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return callback_ids.size() == 2;
        });
    }

    expect(first_response && first_response->starts_with(L"correlated:identical:") &&
               second_response && second_response->starts_with(L"correlated:identical:"),
           L"identical concurrent requests both receive their own response");
    {
        std::lock_guard lock(order_mutex);
        expect(handler_ids.size() == 2 && handler_ids[0] != 0 && handler_ids[1] != 0 &&
                   handler_ids[0] != handler_ids[1],
               L"transport assigns a unique nonzero correlation id to identical requests");
        expect(callback_ids.size() == 2 && callback_ids[0] == second_handler_id &&
                   std::find(callback_ids.begin(), callback_ids.end(), handler_ids[0]) !=
                       callback_ids.end() &&
                   std::find(callback_ids.begin(), callback_ids.end(), handler_ids[1]) !=
                       callback_ids.end() &&
                   all_correlated_sent,
               L"out-of-order completions preserve their transport correlation ids");
    }
    correlated_server.stop();

    std::atomic_int exception_callback_count = 0;
    std::atomic_uint64_t exception_correlation_id = 0;
    std::atomic_bool exception_callback_sent = true;
    airshot::PipeServer throwing_server(test_pipe_name());
    expect(throwing_server.start_correlated(
               [](std::wstring_view, const airshot::PipeRequestContext&) -> std::wstring {
                   throw std::runtime_error("handler failure");
               },
               [&](std::uint64_t id, std::wstring_view request, bool sent) {
                   if (request == L"throw") {
                       exception_correlation_id = id;
                       exception_callback_sent = sent;
                       exception_callback_count.fetch_add(1);
                   }
               }),
           L"throwing-handler server starts");
    expect(!send_pipe_request(L"throw", 1000),
           L"handler exception closes the client without a response");
    const auto exception_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (exception_callback_count.load() == 0 &&
           std::chrono::steady_clock::now() < exception_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(exception_callback_count.load() == 1 && exception_correlation_id.load() != 0 &&
               !exception_callback_sent.load(),
           L"handler exception reports exactly one correlated failed completion");
    throwing_server.stop();
}

void test_client_disconnect_requests_handler_cancellation() {
    std::atomic_bool handler_entered = false;
    std::atomic_bool cancellation_observed = false;
    std::atomic_int callback_count = 0;
    std::atomic_bool callback_sent = true;

    airshot::PipeServer server(test_pipe_name());
    expect(server.start_correlated(
               [&](std::wstring_view, const airshot::PipeRequestContext& context) {
                   handler_entered = true;
                   const auto deadline =
                       std::chrono::steady_clock::now() + std::chrono::seconds(2);
                   while (!context.cancellation_requested() &&
                          std::chrono::steady_clock::now() < deadline) {
                       std::this_thread::sleep_for(std::chrono::milliseconds(5));
                   }
                   cancellation_observed = context.cancellation_requested();
                   return std::wstring(L"late");
               },
               [&](std::uint64_t id, std::wstring_view request, bool sent) {
                   if (id != 0 && request == L"disconnect") {
                       callback_sent = sent;
                       callback_count.fetch_add(1);
                   }
               }),
           L"disconnect-cancellation server starts");

    const auto started = std::chrono::steady_clock::now();
    const auto response = send_pipe_request(L"disconnect", 150);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(!response && elapsed < std::chrono::seconds(1),
           L"client request honors its end-to-end deadline");

    const auto cancellation_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!cancellation_observed.load() || callback_count.load() == 0) &&
           std::chrono::steady_clock::now() < cancellation_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(handler_entered.load() && cancellation_observed.load(),
           L"handler observes that its requesting client disconnected");
    expect(callback_count.load() == 1 && !callback_sent.load(),
           L"disconnected client produces one failed completion callback");
    server.stop();
}

void test_concurrent_stop_is_idempotent() {
    airshot::PipeServer server(test_pipe_name());
    expect(server.start([](std::wstring_view request) { return std::wstring(request); }),
           L"concurrent-stop server starts");

    const auto started = std::chrono::steady_clock::now();
    std::thread first([&] { server.stop(); });
    std::thread second([&] { server.stop(); });
    first.join();
    second.join();

    expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(1),
           L"concurrent stop calls serialize without deadlock or use-after-free");
    expect(!pipe_is_available(0), L"concurrently stopped server releases its pipe");
}

void test_listener_creation_retry_and_fault_notification() {
    std::atomic_int transient_fault_count = 0;
    airshot::PipeServer recovering_server(test_pipe_name(), 1);
    expect(recovering_server.start_correlated(
               [](std::wstring_view request, const airshot::PipeRequestContext&) {
                   return std::wstring(request);
               },
               {},
               [&](DWORD) { transient_fault_count.fetch_add(1); }),
           L"single-instance retry server starts");
    const auto first_response = send_pipe_request(L"first", 2000);
    const auto first_client_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (recovering_server.active_clients() != 0 &&
           std::chrono::steady_clock::now() < first_client_deadline) {
        std::this_thread::yield();
    }
    const auto second_response = send_pipe_request(L"second", 2000);
    expect(first_response && *first_response == L"first",
           L"client served while listener replacement is retried");
    expect(second_response && *second_response == L"second",
           L"listener retries a transient replacement failure");
    expect(transient_fault_count.load() == 0,
           L"successful listener retry does not report a fault");
    recovering_server.stop();

    std::mutex handler_mutex;
    std::condition_variable handler_condition;
    bool handler_entered = false;
    bool release_handler = false;
    std::atomic_int fault_count = 0;
    std::atomic<DWORD> fault_error = ERROR_SUCCESS;

    airshot::PipeServer failing_server(test_pipe_name(), 1);
    expect(failing_server.start_correlated(
               [&](std::wstring_view, const airshot::PipeRequestContext&) {
                   std::unique_lock lock(handler_mutex);
                   handler_entered = true;
                   handler_condition.notify_all();
                   handler_condition.wait(lock, [&] { return release_handler; });
                   return std::wstring(L"drained");
               },
               {},
               [&](DWORD error) {
                   fault_error.store(error);
                   fault_count.fetch_add(1);
               }),
           L"listener-fault server starts");

    std::optional<std::wstring> response;
    std::thread client([&] {
        response = send_pipe_request(L"block replacement", 5000);
    });
    {
        std::unique_lock lock(handler_mutex);
        handler_condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return handler_entered;
        });
    }
    expect(handler_entered, L"listener-fault test occupies the only pipe instance");
    expect(failing_server.wait_until_not_accepting(3000),
           L"listener becomes quiescent after replacement retries are exhausted");
    expect(fault_count.load() == 1 && fault_error.load() != ERROR_SUCCESS,
           L"permanent listener replacement failure is reported exactly once");

    {
        std::lock_guard lock(handler_mutex);
        release_handler = true;
    }
    handler_condition.notify_all();
    client.join();
    expect(response && *response == L"drained",
           L"accepted client drains after the listener faults");
    failing_server.stop();
}

}  // namespace

int wmain() {
    test_round_trip_and_single_server();
    test_restart_and_legacy_start_overload();
    test_stop_cancels_transport_after_handler_cancellation();
    test_stop_accepting_drains_existing_client();
    test_correlated_completion_and_handler_exception();
    test_client_disconnect_requests_handler_cancellation();
    test_concurrent_stop_is_idempotent();
    test_listener_creation_retry_and_fault_notification();
    if (failures == 0) {
        std::wcout << L"All IPC tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
