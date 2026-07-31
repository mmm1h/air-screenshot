#include "airshot/bitmap.h"
#include "airshot/clipboard.h"
#include "airshot/common.h"
#include "airshot/output.h"

#include <shellapi.h>
#include <shlobj_core.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kClipboardOpenAttempts = 8;
constexpr DWORD kClipboardRetryDelayMs = 15;
constexpr int kSkipReturnCode = 77;

class ClipboardOwner {
public:
    ClipboardOwner() noexcept
        : window_(CreateWindowExW(0,
                                  L"STATIC",
                                  L"",
                                  0,
                                  0,
                                  0,
                                  0,
                                  0,
                                  HWND_MESSAGE,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  nullptr)) {}

    ~ClipboardOwner() {
        if (window_) {
            DestroyWindow(window_);
        }
    }

    ClipboardOwner(const ClipboardOwner&) = delete;
    ClipboardOwner& operator=(const ClipboardOwner&) = delete;

    [[nodiscard]] HWND get() const noexcept { return window_; }

private:
    HWND window_{};
};

[[nodiscard]] bool open_clipboard_with_retry(HWND owner, DWORD& last_error) {
    last_error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < kClipboardOpenAttempts; ++attempt) {
        SetLastError(ERROR_SUCCESS);
        if (OpenClipboard(owner)) {
            last_error = ERROR_SUCCESS;
            return true;
        }
        last_error = GetLastError();
        if (attempt + 1 < kClipboardOpenAttempts) {
            Sleep(kClipboardRetryDelayMs);
        }
    }
    return false;
}

void print_open_failure(std::string_view disposition, DWORD error) {
    std::cerr << disposition << ": OpenClipboard failed";
    if (error != ERROR_SUCCESS) {
        std::cerr << " with Win32 " << error << " ("
                  << airshot::to_utf8(airshot::windows_error_message(error)) << ")";
    } else {
        std::cerr << " without extended error information";
    }

    const HWND open_window = GetOpenClipboardWindow();
    if (open_window) {
        DWORD process_id = 0;
        GetWindowThreadProcessId(open_window, &process_id);
        std::cerr << "; open window=0x" << std::hex
                  << reinterpret_cast<std::uintptr_t>(open_window) << std::dec
                  << ", process=" << process_id;
    } else {
        std::cerr << "; open window unavailable";
    }
    std::cerr << '\n';
}

[[nodiscard]] bool close_clipboard_checked(std::string_view phase) {
    SetLastError(ERROR_SUCCESS);
    if (CloseClipboard()) {
        return true;
    }
    const DWORD error = GetLastError();
    std::cerr << "FAIL: CloseClipboard failed during " << phase
              << " with Win32 " << error << " ("
              << airshot::to_utf8(airshot::windows_error_message(error)) << ")\n";
    return false;
}

[[nodiscard]] bool publish_test_dib(HWND owner, UINT format) {
    const bool v5 = format == CF_DIBV5;
    constexpr int width = 2;
    constexpr int height = 2;
    constexpr std::size_t stride = 8;
    const std::size_t header_size =
        v5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
    const std::size_t total_size =
        header_size + stride * height;
    HGLOBAL memory =
        GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total_size);
    if (!memory) {
        return false;
    }
    void* raw = GlobalLock(memory);
    if (!raw) {
        GlobalFree(memory);
        return false;
    }
    if (v5) {
        BITMAPV5HEADER header{};
        header.bV5Size = sizeof(header);
        header.bV5Width = width;
        header.bV5Height = height;
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5SizeImage = static_cast<DWORD>(stride * height);
        header.bV5RedMask = 0x00FF0000;
        header.bV5GreenMask = 0x0000FF00;
        header.bV5BlueMask = 0x000000FF;
        header.bV5AlphaMask = 0xFF000000;
        header.bV5CSType = LCS_sRGB;
        std::memcpy(raw, &header, sizeof(header));
        auto* pixels =
            static_cast<std::uint8_t*>(raw) + sizeof(header);
        for (std::size_t offset = 0;
             offset < stride * height;
             offset += 4) {
            pixels[offset] = 0x33;
            pixels[offset + 1] = 0x66;
            pixels[offset + 2] = 0x99;
            pixels[offset + 3] = 0x80;
        }
    } else {
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = width;
        header.biHeight = height;
        header.biPlanes = 1;
        header.biBitCount = 24;
        header.biCompression = BI_RGB;
        header.biSizeImage = static_cast<DWORD>(stride * height);
        std::memcpy(raw, &header, sizeof(header));
        auto* pixels =
            static_cast<std::uint8_t*>(raw) + sizeof(header);
        for (int row = 0; row < height; ++row) {
            pixels[row * stride] = 0x10;
            pixels[row * stride + 1] = 0x70;
            pixels[row * stride + 2] = 0xD0;
            pixels[row * stride + 3] = 0x20;
            pixels[row * stride + 4] = 0x80;
            pixels[row * stride + 5] = 0xE0;
        }
    }
    GlobalUnlock(memory);

    DWORD open_error = ERROR_SUCCESS;
    if (!open_clipboard_with_retry(owner, open_error)) {
        GlobalFree(memory);
        return false;
    }
    if (!EmptyClipboard() || !SetClipboardData(format, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    if (!CloseClipboard()) {
        return false;
    }
    return true;
}

[[nodiscard]] bool publish_test_bitmap(HWND owner) {
    constexpr int width = 2;
    constexpr int height = 2;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        nullptr,
        &info,
        DIB_RGB_COLORS,
        &bits,
        nullptr,
        0);
    if (!bitmap || !bits) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return false;
    }
    auto* pixels = static_cast<std::uint8_t*>(bits);
    constexpr std::size_t pixel_bytes =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) * 4U;
    for (std::size_t offset = 0;
         offset < pixel_bytes;
         offset += 4) {
        pixels[offset] = 0x22;
        pixels[offset + 1] = 0x88;
        pixels[offset + 2] = 0xEE;
        pixels[offset + 3] = 0;
    }

    DWORD open_error = ERROR_SUCCESS;
    if (!open_clipboard_with_retry(owner, open_error)) {
        DeleteObject(bitmap);
        return false;
    }
    if (!EmptyClipboard() ||
        !SetClipboardData(CF_BITMAP, bitmap)) {
        DeleteObject(bitmap);
        CloseClipboard();
        return false;
    }
    return CloseClipboard() != FALSE;
}

[[nodiscard]] bool publish_corrupt_png_format(HWND owner) {
    constexpr std::array<std::uint8_t, 8> corrupt{
        'n', 'o', 't', '-', 'p', 'n', 'g', 0};
    HGLOBAL memory =
        GlobalAlloc(GMEM_MOVEABLE, corrupt.size());
    if (!memory) {
        return false;
    }
    void* raw = GlobalLock(memory);
    if (!raw) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(raw, corrupt.data(), corrupt.size());
    GlobalUnlock(memory);

    const UINT format = RegisterClipboardFormatW(L"PNG");
    DWORD open_error = ERROR_SUCCESS;
    if (format == 0 ||
        !open_clipboard_with_retry(owner, open_error)) {
        GlobalFree(memory);
        return false;
    }
    if (!SetClipboardData(format, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    return CloseClipboard() != FALSE;
}

[[nodiscard]] bool publish_unterminated_text(
    HWND owner,
    std::size_t character_count) {
    if (character_count == 0 ||
        character_count >
            std::numeric_limits<std::size_t>::max() /
                sizeof(wchar_t)) {
        return false;
    }
    const std::size_t bytes =
        character_count * sizeof(wchar_t);
    HGLOBAL memory =
        GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        return false;
    }
    void* raw = GlobalLock(memory);
    if (!raw) {
        GlobalFree(memory);
        return false;
    }
    std::fill_n(
        static_cast<wchar_t*>(raw),
        character_count,
        L'A');
    GlobalUnlock(memory);

    DWORD open_error = ERROR_SUCCESS;
    if (!open_clipboard_with_retry(owner, open_error)) {
        GlobalFree(memory);
        return false;
    }
    if (!EmptyClipboard() ||
        !SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    return CloseClipboard() != FALSE;
}

[[nodiscard]] bool publish_test_file_drop(
    HWND owner,
    const std::filesystem::path& path) {
    const std::wstring native = path.wstring();
    const std::size_t character_count = native.size() + 2;
    const std::size_t total_size =
        sizeof(DROPFILES) + character_count * sizeof(wchar_t);
    HGLOBAL memory =
        GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total_size);
    if (!memory) {
        return false;
    }
    void* raw = GlobalLock(memory);
    if (!raw) {
        GlobalFree(memory);
        return false;
    }
    auto* drop = static_cast<DROPFILES*>(raw);
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    auto* characters = reinterpret_cast<wchar_t*>(
        static_cast<std::uint8_t*>(raw) + sizeof(DROPFILES));
    std::memcpy(
        characters,
        native.c_str(),
        (native.size() + 1) * sizeof(wchar_t));
    characters[native.size() + 1] = L'\0';
    GlobalUnlock(memory);

    DWORD open_error = ERROR_SUCCESS;
    if (!open_clipboard_with_retry(owner, open_error)) {
        GlobalFree(memory);
        return false;
    }
    if (!EmptyClipboard() ||
        !SetClipboardData(CF_HDROP, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    return CloseClipboard() != FALSE;
}

}  // namespace

int wmain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    SetLastError(ERROR_SUCCESS);
    ClipboardOwner owner;
    if (!owner.get()) {
        const DWORD error = GetLastError();
        std::cerr << "SKIP: clipboard integration test cannot create its owner window"
                  << "; Win32 " << error << " ("
                  << airshot::to_utf8(airshot::windows_error_message(error)) << ")\n";
        return kSkipReturnCode;
    }

    DWORD open_error = ERROR_SUCCESS;
    if (!open_clipboard_with_retry(owner.get(), open_error)) {
        print_open_failure("SKIP: system clipboard is unavailable", open_error);
        return kSkipReturnCode;
    }
    if (!close_clipboard_checked("availability probe")) {
        return 1;
    }

    airshot::Bitmap source(2, 2);
    for (std::size_t index = 0; index < source.pixels.size(); ++index) {
        source.pixels[index] = 255;
    }

    std::wstring copy_error;
    if (!airshot::copy_bitmap_to_clipboard(owner.get(), source, &copy_error)) {
        std::cerr << "FAIL: copy_bitmap_to_clipboard failed after the availability probe";
        if (!copy_error.empty()) {
            std::cerr << ": " << airshot::to_utf8(copy_error);
        }
        std::cerr << '\n';
        return 1;
    }

    std::wstring read_error;
    const auto image_visual =
        airshot::read_clipboard_visual(owner.get(), &read_error);
    if (!image_visual ||
        image_visual->kind != airshot::ClipboardVisualKind::image ||
        image_visual->bitmap.width != source.width ||
        image_visual->bitmap.height != source.height ||
        image_visual->bitmap.pixels != source.pixels) {
        std::cerr << "FAIL: clipboard image cannot be read back as a pin visual";
        if (!read_error.empty()) {
            std::cerr << ": " << airshot::to_utf8(read_error);
        }
        std::cerr << '\n';
        return 1;
    }

    if (!open_clipboard_with_retry(owner.get(), open_error)) {
        print_open_failure("FAIL: clipboard format readback is unavailable", open_error);
        return 1;
    }

    bool has_png = false;
    bool has_image_png = false;
    DWORD enumeration_error = ERROR_SUCCESS;
    UINT format = 0;
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        format = EnumClipboardFormats(format);
        if (format == 0) {
            enumeration_error = GetLastError();
            break;
        }

        wchar_t name[256]{};
        if (GetClipboardFormatNameW(format, name, static_cast<int>(std::size(name))) > 0) {
            const std::wstring_view format_name(name);
            has_png = has_png || format_name == L"PNG";
            has_image_png = has_image_png || format_name == L"image/png";
        }
    }

    const bool close_succeeded = close_clipboard_checked("format readback");
    if (enumeration_error != ERROR_SUCCESS) {
        std::cerr << "FAIL: EnumClipboardFormats failed with Win32 "
                  << enumeration_error << " ("
                  << airshot::to_utf8(
                         airshot::windows_error_message(enumeration_error))
                  << ")\n";
        return 1;
    }
    if (!close_succeeded) {
        return 1;
    }
    if (!has_png || !has_image_png) {
        std::cerr << "FAIL: clipboard formats are incomplete; PNG="
                  << has_png << ", image/png=" << has_image_png << '\n';
        return 1;
    }

    if (!publish_test_bitmap(owner.get())) {
        std::cerr << "FAIL: cannot seed CF_BITMAP clipboard data\n";
        return 1;
    }
    const auto bitmap_visual =
        airshot::read_clipboard_visual(owner.get(), &read_error);
    if (!bitmap_visual ||
        bitmap_visual->kind !=
            airshot::ClipboardVisualKind::image ||
        bitmap_visual->bitmap.width != 2 ||
        bitmap_visual->bitmap.height != 2) {
        std::cerr
            << "FAIL: CF_BITMAP cannot be read as a pin visual";
        if (!read_error.empty()) {
            std::cerr << ": " << airshot::to_utf8(read_error);
        }
        std::cerr << '\n';
        return 1;
    }

    for (const UINT dib_format : {CF_DIBV5, CF_DIB}) {
        if (!publish_test_dib(owner.get(), dib_format)) {
            std::cerr << "FAIL: cannot seed DIB clipboard format "
                      << dib_format << '\n';
            return 1;
        }
        const auto dib_visual =
            airshot::read_clipboard_visual(owner.get(), &read_error);
        if (!dib_visual ||
            dib_visual->kind != airshot::ClipboardVisualKind::image ||
            dib_visual->bitmap.width != 2 ||
            dib_visual->bitmap.height != 2 ||
            dib_visual->bitmap.pixels.size() < 4 ||
            (dib_format == CF_DIBV5 &&
             (dib_visual->bitmap.pixels[0] != 149 ||
              dib_visual->bitmap.pixels[1] != 175 ||
              dib_visual->bitmap.pixels[2] != 200 ||
              dib_visual->bitmap.pixels[3] != 0xFF)) ||
            (dib_format == CF_DIB &&
             (dib_visual->bitmap.pixels[0] != 0x10 ||
              dib_visual->bitmap.pixels[1] != 0x70 ||
              dib_visual->bitmap.pixels[2] != 0xD0 ||
              dib_visual->bitmap.pixels[3] != 0xFF))) {
            std::cerr << "FAIL: DIB clipboard format "
                      << dib_format
                      << " cannot be read as a pin visual";
            if (!read_error.empty()) {
                std::cerr << ": " << airshot::to_utf8(read_error);
            }
            std::cerr << '\n';
            return 1;
        }
    }

    if (!publish_test_dib(owner.get(), CF_DIB) ||
        !publish_corrupt_png_format(owner.get())) {
        std::cerr
            << "FAIL: cannot seed corrupt PNG with valid DIB fallback\n";
        return 1;
    }
    const auto fallback_visual =
        airshot::read_clipboard_visual(owner.get(), &read_error);
    if (!fallback_visual ||
        fallback_visual->bitmap.width != 2 ||
        fallback_visual->bitmap.height != 2 ||
        fallback_visual->bitmap.pixels.size() < 4 ||
        fallback_visual->bitmap.pixels[0] != 0x10 ||
        fallback_visual->bitmap.pixels[1] != 0x70 ||
        fallback_visual->bitmap.pixels[2] != 0xD0) {
        std::cerr
            << "FAIL: corrupt preferred PNG does not fall back to valid DIB";
        if (!read_error.empty()) {
            std::cerr << ": " << airshot::to_utf8(read_error);
        }
        std::cerr << '\n';
        return 1;
    }

    constexpr std::size_t oversized_text_characters =
        64U * 1024U + 4096U;
    if (!publish_unterminated_text(
            owner.get(),
            oversized_text_characters)) {
        std::cerr
            << "FAIL: cannot seed oversized unterminated clipboard text\n";
        return 1;
    }
    const auto bounded_text_visual =
        airshot::read_clipboard_visual(owner.get(), &read_error);
    if (!bounded_text_visual ||
        bounded_text_visual->kind !=
            airshot::ClipboardVisualKind::text ||
        !bounded_text_visual->bitmap.valid()) {
        std::cerr
            << "FAIL: oversized clipboard text is not safely bounded";
        if (!read_error.empty()) {
            std::cerr << ": " << airshot::to_utf8(read_error);
        }
        std::cerr << '\n';
        return 1;
    }

    const std::filesystem::path file_drop_path =
        std::filesystem::temp_directory_path() /
        (L"airshot-clipboard-" +
         std::to_wstring(GetCurrentProcessId()) +
         L".png");
    std::wstring file_error;
    if (!airshot::save_png(source, file_drop_path, &file_error) ||
        !publish_test_file_drop(owner.get(), file_drop_path)) {
        std::error_code cleanup_error;
        std::filesystem::remove(file_drop_path, cleanup_error);
        std::cerr << "FAIL: cannot seed local image file clipboard data";
        if (!file_error.empty()) {
            std::cerr << ": " << airshot::to_utf8(file_error);
        }
        std::cerr << '\n';
        return 1;
    }
    const auto file_visual =
        airshot::read_clipboard_visual(owner.get(), &read_error);
    std::error_code cleanup_error;
    std::filesystem::remove(file_drop_path, cleanup_error);
    if (!file_visual ||
        file_visual->kind !=
            airshot::ClipboardVisualKind::image_file ||
        file_visual->bitmap.width != source.width ||
        file_visual->bitmap.height != source.height ||
        file_visual->bitmap.pixels != source.pixels) {
        std::cerr
            << "FAIL: copied local image file cannot be read as a pin visual";
        if (!read_error.empty()) {
            std::cerr << ": " << airshot::to_utf8(read_error);
        }
        std::cerr << '\n';
        return 1;
    }

    std::wstring text_error;
    if (!airshot::copy_text_to_clipboard(
            owner.get(),
            L"#336699",
            &text_error)) {
        std::cerr << "FAIL: cannot seed color text for clipboard pin test: "
                  << airshot::to_utf8(text_error) << '\n';
        return 1;
    }
    const auto color_visual =
        airshot::read_clipboard_visual(owner.get(), &read_error);
    if (!color_visual ||
        color_visual->kind != airshot::ClipboardVisualKind::color ||
        !color_visual->bitmap.valid() ||
        color_visual->description != L"#336699") {
        std::cerr << "FAIL: CSS hex color is not converted into a pin visual";
        if (!read_error.empty()) {
            std::cerr << ": " << airshot::to_utf8(read_error);
        }
        std::cerr << '\n';
        return 1;
    }

    if (!airshot::copy_text_to_clipboard(
            owner.get(),
            L"Air Screenshot\r\nclipboard note",
            &text_error)) {
        std::cerr << "FAIL: cannot seed text for clipboard pin test: "
                  << airshot::to_utf8(text_error) << '\n';
        return 1;
    }
    const auto text_visual =
        airshot::read_clipboard_visual(owner.get(), &read_error);
    if (!text_visual ||
        text_visual->kind != airshot::ClipboardVisualKind::text ||
        !text_visual->bitmap.valid() ||
        text_visual->bitmap.width < 280 ||
        text_visual->bitmap.height < 84) {
        std::cerr << "FAIL: Unicode text is not converted into a readable pin card";
        if (!read_error.empty()) {
            std::cerr << ": " << airshot::to_utf8(read_error);
        }
        std::cerr << '\n';
        return 1;
    }

    std::cout << "All clipboard integration tests passed.\n";
    return 0;
}
