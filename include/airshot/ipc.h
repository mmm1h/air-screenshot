#pragma once

#include "airshot/common.h"

#include <atomic>
#include <functional>
#include <thread>

namespace airshot {

[[nodiscard]] bool pipe_is_available(DWORD timeout_ms = 0);
[[nodiscard]] std::optional<std::wstring> send_pipe_request(
    std::wstring_view request_json,
    DWORD timeout_ms = 120000);

class PipeServer {
public:
    using Handler = std::function<std::wstring(std::wstring_view)>;

    PipeServer() = default;
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    bool start(Handler handler);
    void stop();

private:
    void run();

    Handler handler_;
    std::atomic_bool stopping_{false};
    std::thread thread_;
};

}  // namespace airshot
