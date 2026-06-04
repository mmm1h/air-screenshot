#pragma once

#include "airshot/bitmap.h"

namespace airshot {

[[nodiscard]] bool copy_bitmap_to_clipboard(const Bitmap& bitmap, std::wstring* error = nullptr);
[[nodiscard]] bool copy_text_to_clipboard(std::wstring_view text, std::wstring* error = nullptr);
[[nodiscard]] bool save_png(const Bitmap& bitmap, const std::filesystem::path& path, std::wstring* error = nullptr);
[[nodiscard]] std::filesystem::path resolve_output_path(std::wstring_view requested);
[[nodiscard]] std::optional<std::filesystem::path> prompt_png_path(HWND owner);

}  // namespace airshot
