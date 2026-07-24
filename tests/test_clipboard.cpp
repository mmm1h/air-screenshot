#include "airshot/bitmap.h"
#include "airshot/common.h"
#include "airshot/output.h"

#include <winrt/base.h>

#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

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

    std::cout << "All clipboard integration tests passed.\n";
    return 0;
}
