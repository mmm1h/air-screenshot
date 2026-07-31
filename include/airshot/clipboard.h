#pragma once

#include "airshot/bitmap.h"

namespace airshot {

enum class ClipboardVisualKind {
    image,
    image_file,
    color,
    text,
};

struct ClipboardVisual {
    Bitmap bitmap;
    ClipboardVisualKind kind{ClipboardVisualKind::image};
    std::wstring description;
};

// Converts the most useful clipboard payload into a visual that can be pinned.
// Priority is image data, a dropped image file, then Unicode text (including
// CSS-style hex/rgb colors). The returned bitmap is top-down BGRA8.
[[nodiscard]] std::optional<ClipboardVisual> read_clipboard_visual(
    HWND owner = nullptr,
    std::wstring* error = nullptr);

}  // namespace airshot
