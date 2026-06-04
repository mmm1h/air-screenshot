#pragma once

#include "airshot/bitmap.h"

namespace airshot {

struct OcrOutput {
    bool ok{};
    std::wstring text;
    std::wstring error;
};

[[nodiscard]] OcrOutput recognize_text(const Bitmap& bitmap);
[[nodiscard]] std::wstring join_ocr_lines(std::span<const std::wstring> lines);

}  // namespace airshot
