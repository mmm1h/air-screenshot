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
