#pragma once

#include "airshot/bitmap.h"

namespace airshot {

struct AppConfig;

struct OcrOutput {
    bool ok{};
    std::wstring text;
    std::wstring error;
};

[[nodiscard]] OcrOutput recognize_text(const Bitmap& bitmap, const AppConfig& config);
[[nodiscard]] std::wstring join_ocr_lines(std::span<const std::wstring> lines);

}  // namespace airshot
