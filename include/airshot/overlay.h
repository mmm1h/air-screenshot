#pragma once

#include "airshot/capture.h"
#include "airshot/config.h"

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

[[nodiscard]] RegionResult run_region_capture(const RegionRequest& request);

}  // namespace airshot
