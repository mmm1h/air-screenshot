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

// Decodes the first frame of a local static image through WIC. Network paths,
// files over 128 MiB and decoded images over 64 MiB are rejected.
[[nodiscard]] std::optional<Bitmap> decode_local_image_file(
    const std::filesystem::path& path,
    std::wstring* error = nullptr);

}  // namespace airshot
