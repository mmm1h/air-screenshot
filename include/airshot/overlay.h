#pragma once

#include "airshot/capture.h"
#include "airshot/config.h"

#include <functional>
#include <memory>

namespace airshot {

enum class RegionAction {
    interactive,
    clipboard,
    file,
    ocr,
    pin,
};

[[nodiscard]] constexpr RegionAction resolve_region_result_action(
    ExitCode code,
    RegionAction result_action,
    RegionAction request_action) noexcept {
    // A successful interactive result means that the completing feature did
    // not request a host-side action. Never turn it into a pin merely because
    // the session itself was launched by the pin command.
    if (code == ExitCode::success ||
        result_action != RegionAction::interactive) {
        return result_action;
    }
    return request_action;
}

struct RegionRequest {
    RegionAction action{RegionAction::interactive};
    std::wstring path;
    bool copy_ocr{true};
    AppConfig config;
};

struct RegionResult {
    ExitCode code{ExitCode::user_cancelled};
    std::wstring message;
    std::wstring path;
    std::wstring text;
    RegionAction action{RegionAction::interactive};
    Bitmap bitmap;
    RectI bounds;
    AppConfig config;
    std::wstring topology_signature;
};

using RegionCaptureCompletion = std::function<void(RegionResult)>;

class RegionCaptureSession {
public:
    ~RegionCaptureSession();
    RegionCaptureSession(const RegionCaptureSession&) = delete;
    RegionCaptureSession& operator=(const RegionCaptureSession&) = delete;

    void cancel();
    [[nodiscard]] bool active() const noexcept;

private:
    struct Impl;
    explicit RegionCaptureSession(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend std::unique_ptr<RegionCaptureSession> start_region_capture(
        RegionRequest request, RegionCaptureCompletion completion);
};

[[nodiscard]] std::unique_ptr<RegionCaptureSession> start_region_capture(
    RegionRequest request, RegionCaptureCompletion completion);
[[nodiscard]] RegionResult run_region_capture(const RegionRequest& request);

}  // namespace airshot
