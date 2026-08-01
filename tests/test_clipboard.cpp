#include "airshot/bitmap.h"
#include "airshot/clipboard.h"
#include "airshot/common.h"
#include "airshot/output.h"
#include "output_test_support.h"

#include <shellapi.h>
#include <shlobj_core.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <thread>
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

[[nodiscard]] bool read_unicode_clipboard_text(
    HWND owner,
    std::wstring& text) {
    text.clear();
    DWORD open_error = ERROR_SUCCESS;
    if (!open_clipboard_with_retry(owner, open_error)) {
        return false;
    }

    const HGLOBAL memory = static_cast<HGLOBAL>(
        GetClipboardData(CF_UNICODETEXT));
    const SIZE_T bytes = memory ? GlobalSize(memory) : 0;
    const auto* characters = memory
        ? static_cast<const wchar_t*>(GlobalLock(memory))
        : nullptr;
    bool succeeded = false;
    if (characters && bytes >= sizeof(wchar_t)) {
        const std::size_t capacity = bytes / sizeof(wchar_t);
        const wchar_t* terminator = std::find(
            characters,
            characters + capacity,
            L'\0');
        if (terminator != characters + capacity) {
            text.assign(characters, terminator);
            succeeded = true;
        }
    }
    if (characters) {
        GlobalUnlock(memory);
    }
    return CloseClipboard() != FALSE && succeeded;
}

[[nodiscard]] bool publish_unicode_clipboard_text(
    HWND owner,
    std::wstring_view text) {
    if (text.size() >
        std::numeric_limits<std::size_t>::max() / sizeof(wchar_t) - 1) {
        return false;
    }
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory) {
        return false;
    }
    auto* characters = static_cast<wchar_t*>(GlobalLock(memory));
    if (!characters) {
        GlobalFree(memory);
        return false;
    }
    if (!text.empty()) {
        std::memcpy(
            characters,
            text.data(),
            text.size() * sizeof(wchar_t));
    }
    characters[text.size()] = L'\0';
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

class StaDelayedTextDataObject final : public IDataObject {
public:
    StaDelayedTextDataObject(
        std::wstring_view text,
        DWORD owner_thread,
        std::atomic_uint& callbacks,
        std::atomic_uint& get_data_callbacks,
        std::atomic_bool& wrong_thread) noexcept
        : text_(text),
          owner_thread_(owner_thread),
          callbacks_(callbacks),
          get_data_callbacks_(get_data_callbacks),
          wrong_thread_(wrong_thread) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interface_id,
        void** object) noexcept override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (InlineIsEqualGUID(interface_id, IID_IUnknown) ||
            InlineIsEqualGUID(interface_id, IID_IDataObject)) {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG remaining =
            references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetData(
        FORMATETC* requested,
        STGMEDIUM* medium) noexcept override {
        record_callback();
        get_data_callbacks_.fetch_add(1, std::memory_order_relaxed);
        if (!requested || !medium) {
            return E_INVALIDARG;
        }
        *medium = {};
        const HRESULT query_result = QueryGetData(requested);
        if (FAILED(query_result)) {
            return query_result;
        }

        const std::size_t bytes =
            (text_.size() + 1) * sizeof(wchar_t);
        const HGLOBAL memory =
            GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!memory) {
            return STG_E_MEDIUMFULL;
        }
        auto* characters = static_cast<wchar_t*>(GlobalLock(memory));
        if (!characters) {
            GlobalFree(memory);
            return STG_E_MEDIUMFULL;
        }
        if (!text_.empty()) {
            std::memcpy(
                characters,
                text_.data(),
                text_.size() * sizeof(wchar_t));
        }
        characters[text_.size()] = L'\0';
        GlobalUnlock(memory);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(
        FORMATETC*,
        STGMEDIUM*) noexcept override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(
        FORMATETC* requested) noexcept override {
        if (!requested) {
            return E_INVALIDARG;
        }
        if (requested->cfFormat != CF_UNICODETEXT) {
            return DV_E_FORMATETC;
        }
        if (requested->dwAspect != DVASPECT_CONTENT) {
            return DV_E_DVASPECT;
        }
        if (requested->lindex != -1) {
            return DV_E_LINDEX;
        }
        if (requested->ptd) {
            return DV_E_DVTARGETDEVICE;
        }
        return (requested->tymed & TYMED_HGLOBAL) != 0
                   ? S_OK
                   : DV_E_TYMED;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
        FORMATETC*,
        FORMATETC* canonical) noexcept override {
        if (!canonical) {
            return E_INVALIDARG;
        }
        canonical->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(
        FORMATETC*,
        STGMEDIUM*,
        BOOL) noexcept override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(
        DWORD direction,
        IEnumFORMATETC** enumerator) noexcept override {
        record_callback();
        if (!enumerator) {
            return E_POINTER;
        }
        *enumerator = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        FORMATETC descriptor{
            CF_UNICODETEXT,
            nullptr,
            DVASPECT_CONTENT,
            -1,
            TYMED_HGLOBAL,
        };
        return SHCreateStdEnumFmtEtc(1, &descriptor, enumerator);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(
        FORMATETC*,
        DWORD,
        IAdviseSink*,
        DWORD* connection) noexcept override {
        if (connection) {
            *connection = 0;
        }
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) noexcept override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(
        IEnumSTATDATA** enumerator) noexcept override {
        if (enumerator) {
            *enumerator = nullptr;
        }
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    void record_callback() noexcept {
        callbacks_.fetch_add(1, std::memory_order_relaxed);
        if (GetCurrentThreadId() != owner_thread_) {
            wrong_thread_.store(true, std::memory_order_relaxed);
        }
    }

    std::atomic<ULONG> references_{1};
    std::wstring_view text_;
    DWORD owner_thread_{};
    std::atomic_uint& callbacks_;
    std::atomic_uint& get_data_callbacks_;
    std::atomic_bool& wrong_thread_;
};

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
    // Ensure the preferred PNG/CF_DIBV5 round trip keeps straight alpha;
    // legacy CF_DIB/CF_BITMAP remain separately checked for availability.
    source.pixels[0] = 0x21;
    source.pixels[1] = 0x43;
    source.pixels[2] = 0x65;
    source.pixels[3] = 0x80;

    std::wstring copy_error;
    std::atomic_uint sta_callbacks{};
    std::atomic_uint sta_get_data_callbacks{};
    std::atomic_bool sta_wrong_thread{};
    HRESULT sta_initialize_result = E_FAIL;
    HRESULT sta_publish_result = E_FAIL;
    bool sta_copy_succeeded = false;
    std::wstring sta_copy_error;
    airshot::output_test::set_clipboard_wait_timeout_for_testing(1'500);
    std::thread sta_source_thread([&]() noexcept {
        sta_initialize_result = OleInitialize(nullptr);
        if (FAILED(sta_initialize_result)) {
            return;
        }
        auto* delayed_source = new (std::nothrow) StaDelayedTextDataObject(
            L"Air Screenshot same-STA delayed source",
            GetCurrentThreadId(),
            sta_callbacks,
            sta_get_data_callbacks,
            sta_wrong_thread);
        if (!delayed_source) {
            sta_publish_result = E_OUTOFMEMORY;
            OleUninitialize();
            return;
        }

        sta_publish_result = OleSetClipboard(delayed_source);
        if (SUCCEEDED(sta_publish_result)) {
            sta_copy_succeeded = airshot::copy_text_to_clipboard(
                nullptr,
                L"Air Screenshot STA callback replacement",
                &sta_copy_error);
        }
        if (OleIsCurrentClipboard(delayed_source) == S_OK) {
            (void)OleSetClipboard(nullptr);
        }
        delayed_source->Release();
        OleUninitialize();
    });
    sta_source_thread.join();
    airshot::output_test::set_clipboard_wait_timeout_for_testing(0);
    if (FAILED(sta_initialize_result) ||
        FAILED(sta_publish_result) ||
        !sta_copy_succeeded ||
        sta_callbacks.load(std::memory_order_relaxed) == 0 ||
        sta_get_data_callbacks.load(std::memory_order_relaxed) == 0 ||
        sta_wrong_thread.load(std::memory_order_relaxed)) {
        std::cerr
            << "FAIL: same-STA delayed IDataObject was not materialized "
               "through the bounded COM-dispatch wait; init=0x"
            << std::hex << static_cast<unsigned long>(sta_initialize_result)
            << ", publish=0x"
            << static_cast<unsigned long>(sta_publish_result)
            << std::dec << ", callbacks="
            << sta_callbacks.load(std::memory_order_relaxed)
            << ", GetData callbacks="
            << sta_get_data_callbacks.load(std::memory_order_relaxed)
            << ", wrong thread="
            << sta_wrong_thread.load(std::memory_order_relaxed)
            << ", error=" << airshot::to_utf8(sta_copy_error) << '\n';
        return 1;
    }

    constexpr std::wstring_view sentinel =
        L"Air Screenshot clipboard transaction sentinel";
    if (!airshot::copy_text_to_clipboard(
            owner.get(), sentinel, &copy_error)) {
        std::cerr << "FAIL: cannot seed transactional clipboard sentinel: "
                  << airshot::to_utf8(copy_error) << '\n';
        return 1;
    }

    constexpr std::wstring_view snapshot_unavailable_replacement =
        L"Air Screenshot snapshot-unavailable replacement";
    airshot::output_test::set_clipboard_snapshot_failure_for_testing(true);
    copy_error.clear();
    const bool snapshot_unavailable_copy_succeeded =
        airshot::copy_text_to_clipboard(
            owner.get(),
            snapshot_unavailable_replacement,
            &copy_error);
    airshot::output_test::set_clipboard_snapshot_failure_for_testing(false);
    std::wstring snapshot_unavailable_text;
    if (!snapshot_unavailable_copy_succeeded ||
        !copy_error.empty() ||
        !read_unicode_clipboard_text(
            owner.get(),
            snapshot_unavailable_text) ||
        snapshot_unavailable_text != snapshot_unavailable_replacement) {
        std::cerr
            << "FAIL: unavailable old-clipboard snapshot blocked a successful "
               "forward publication; error="
            << airshot::to_utf8(copy_error) << '\n';
        return 1;
    }
    if (!airshot::copy_text_to_clipboard(
            owner.get(), sentinel, &copy_error)) {
        std::cerr
            << "FAIL: cannot restore the transaction sentinel after the "
               "snapshot-unavailable regression: "
            << airshot::to_utf8(copy_error) << '\n';
        return 1;
    }

    airshot::output_test::
        set_required_clipboard_format_failure_for_testing(2);
    const bool second_format_rejected =
        !airshot::copy_bitmap_to_clipboard(
            owner.get(), source, &copy_error);
    airshot::output_test::
        set_required_clipboard_format_failure_for_testing(0);
    std::wstring preserved_text;
    if (!second_format_rejected ||
        !read_unicode_clipboard_text(owner.get(), preserved_text) ||
        preserved_text != sentinel) {
        std::cerr
            << "FAIL: a staged failure in the second required image format "
               "modified the existing clipboard\n";
        return 1;
    }

    airshot::output_test::
        set_required_clipboard_format_failure_for_testing(1);
    const bool text_format_rejected =
        !airshot::copy_text_to_clipboard(
            owner.get(), L"replacement", &copy_error);
    airshot::output_test::
        set_required_clipboard_format_failure_for_testing(0);
    if (!text_format_rejected ||
        !read_unicode_clipboard_text(owner.get(), preserved_text) ||
        preserved_text != sentinel) {
        std::cerr
            << "FAIL: a staged Unicode text failure modified the existing "
               "clipboard\n";
        return 1;
    }

    copy_error.clear();
    airshot::output_test::set_clipboard_flush_failure_for_testing(true);
    const bool failed_flush_rejected =
        !airshot::copy_text_to_clipboard(
            owner.get(), L"flush replacement", &copy_error);
    airshot::output_test::set_clipboard_flush_failure_for_testing(false);
    preserved_text.clear();
    if (!failed_flush_rejected ||
        copy_error.find(L"已恢复") == std::wstring::npos ||
        !read_unicode_clipboard_text(owner.get(), preserved_text) ||
        preserved_text != sentinel) {
        std::cerr
            << "FAIL: an injected OleFlushClipboard failure did not restore "
               "the prior sentinel; error="
            << airshot::to_utf8(copy_error) << '\n';
        return 1;
    }

    constexpr std::wstring_view concurrent_sentinel =
        L"Air Screenshot newer concurrent clipboard content";
    std::atomic_bool publisher_observed_forward_set{};
    std::atomic_bool publisher_succeeded{};
    // Store sequence + 1 so the valid DWORD sequence value zero remains
    // distinguishable from "the publisher never completed".
    std::atomic<std::uint64_t> publisher_sequence_marker{};
    airshot::output_test::set_clipboard_forward_set_gate_for_testing(true);
    std::thread concurrent_publisher([&]() noexcept {
        const ULONGLONG deadline = GetTickCount64() + 2'000;
        while (!airshot::output_test::
                    clipboard_forward_set_pending_for_testing() &&
               GetTickCount64() < deadline) {
            Sleep(1);
        }
        if (!airshot::output_test::
                 clipboard_forward_set_pending_for_testing()) {
            airshot::output_test::set_clipboard_forward_set_gate_for_testing(
                false);
            return;
        }
        publisher_observed_forward_set.store(
            true,
            std::memory_order_release);
        const bool succeeded = publish_unicode_clipboard_text(
            owner.get(),
            concurrent_sentinel);
        publisher_succeeded.store(succeeded, std::memory_order_release);
        if (succeeded) {
            publisher_sequence_marker.store(
                static_cast<std::uint64_t>(
                    GetClipboardSequenceNumber()) + 1,
                std::memory_order_release);
        }
        // Release the Air Screenshot worker only after this publication has
        // either completed or definitively failed. This proves its subsequent
        // ownership check cannot roll back a fully-published newer value.
        airshot::output_test::set_clipboard_forward_set_gate_for_testing(
            false);
    });
    copy_error.clear();
    const bool superseded_copy_rejected =
        !airshot::copy_text_to_clipboard(
            owner.get(),
            L"must not replace the concurrent publisher",
            &copy_error);
    concurrent_publisher.join();
    airshot::output_test::set_clipboard_forward_set_gate_for_testing(false);
    std::wstring concurrent_text;
    const bool concurrent_read_succeeded =
        read_unicode_clipboard_text(owner.get(), concurrent_text);
    const std::uint64_t published_sequence_marker =
        publisher_sequence_marker.load(std::memory_order_acquire);
    const DWORD published_sequence = published_sequence_marker == 0
                                         ? 0
                                         : static_cast<DWORD>(
                                               published_sequence_marker - 1);
    const DWORD final_sequence = GetClipboardSequenceNumber();
    if (!superseded_copy_rejected ||
        !publisher_observed_forward_set.load(std::memory_order_acquire) ||
        !publisher_succeeded.load(std::memory_order_acquire) ||
        copy_error.find(L"较新") == std::wstring::npos ||
        published_sequence_marker == 0 ||
        final_sequence != published_sequence ||
        !concurrent_read_succeeded ||
        concurrent_text != concurrent_sentinel) {
        std::cerr
            << "FAIL: rollback overwrote a newer concurrent clipboard "
               "publisher; error="
            << airshot::to_utf8(copy_error)
            << ", observed="
            << publisher_observed_forward_set.load(std::memory_order_acquire)
            << ", published="
            << publisher_succeeded.load(std::memory_order_acquire)
            << ", read=" << concurrent_read_succeeded
            << ", publisher sequence=" << published_sequence
            << ", final sequence=" << final_sequence
            << ", final text=" << airshot::to_utf8(concurrent_text) << '\n';
        return 1;
    }

    constexpr std::uint32_t worker_delay_ms = 400;
    constexpr std::uint32_t bounded_wait_ms = 20;
    airshot::output_test::set_clipboard_worker_delay_for_testing(
        worker_delay_ms);
    airshot::output_test::set_clipboard_wait_timeout_for_testing(
        bounded_wait_ms);
    copy_error.clear();
    const ULONGLONG timeout_started = GetTickCount64();
    const bool delayed_commit_timed_out =
        !airshot::copy_text_to_clipboard(
            owner.get(), L"background clipboard commit", &copy_error);
    const ULONGLONG timeout_elapsed =
        GetTickCount64() - timeout_started;
    const bool preparation_token_released =
        !airshot::output_test::clipboard_commit_in_flight_for_testing();
    airshot::output_test::set_clipboard_worker_delay_for_testing(0);
    airshot::output_test::set_clipboard_wait_timeout_for_testing(0);

    constexpr std::wstring_view post_timeout_replacement =
        L"Air Screenshot commit after canceled preparation";
    std::wstring retry_error;
    const bool retry_succeeded = airshot::copy_text_to_clipboard(
        owner.get(),
        post_timeout_replacement,
        &retry_error);
    Sleep(worker_delay_ms + 300);
    std::wstring post_timeout_text;
    const bool late_write_prevented = read_unicode_clipboard_text(
        owner.get(),
        post_timeout_text) &&
        post_timeout_text == post_timeout_replacement;
    if (!delayed_commit_timed_out || timeout_elapsed >= worker_delay_ms ||
        copy_error.find(L"已取消") == std::wstring::npos ||
        !preparation_token_released || !retry_succeeded ||
        !retry_error.empty() || !late_write_prevented) {
        std::cerr
            << "FAIL: a timed-out preparing generation remained busy or "
               "performed a late write; timeout ms="
            << timeout_elapsed << ", timeout error="
            << airshot::to_utf8(copy_error) << ", retry error="
            << airshot::to_utf8(retry_error) << '\n';
        return 1;
    }

    constexpr std::wstring_view mutating_timeout_replacement =
        L"Air Screenshot mutating timeout replacement";
    airshot::output_test::set_clipboard_pre_flush_delay_for_testing(300);
    airshot::output_test::set_clipboard_wait_timeout_for_testing(20);
    std::wstring mutating_timeout_error;
    const bool mutating_commit_timed_out =
        !airshot::copy_text_to_clipboard(
            owner.get(),
            mutating_timeout_replacement,
            &mutating_timeout_error);
    const bool mutating_remained_in_flight =
        airshot::output_test::clipboard_commit_in_flight_for_testing();
    std::wstring mutating_busy_error;
    const bool mutating_concurrent_rejected =
        !airshot::copy_text_to_clipboard(
            owner.get(),
            L"must remain busy while mutating",
            &mutating_busy_error);
    const ULONGLONG mutating_deadline = GetTickCount64() + 2'000;
    while (airshot::output_test::clipboard_commit_in_flight_for_testing() &&
           GetTickCount64() < mutating_deadline) {
        Sleep(10);
    }
    const bool mutating_completed =
        !airshot::output_test::clipboard_commit_in_flight_for_testing();
    airshot::output_test::set_clipboard_pre_flush_delay_for_testing(0);
    airshot::output_test::set_clipboard_wait_timeout_for_testing(0);
    std::wstring mutating_text;
    if (!mutating_commit_timed_out ||
        mutating_timeout_error.find(L"后台") == std::wstring::npos ||
        !mutating_remained_in_flight ||
        !mutating_concurrent_rejected ||
        mutating_busy_error.find(L"已有") == std::wstring::npos ||
        !mutating_completed ||
        !read_unicode_clipboard_text(owner.get(), mutating_text) ||
        mutating_text != mutating_timeout_replacement) {
        std::cerr
            << "FAIL: a mutating timed-out generation did not retain its "
               "exclusive token through real completion; timeout error="
            << airshot::to_utf8(mutating_timeout_error)
            << ", busy error="
            << airshot::to_utf8(mutating_busy_error) << '\n';
        return 1;
    }

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
    const UINT png_format = RegisterClipboardFormatW(L"PNG");
    const UINT image_png_format = RegisterClipboardFormatW(L"image/png");
    const HGLOBAL png_data = png_format != 0
        ? static_cast<HGLOBAL>(GetClipboardData(png_format))
        : nullptr;
    const HGLOBAL image_png_data = image_png_format != 0
        ? static_cast<HGLOBAL>(GetClipboardData(image_png_format))
        : nullptr;
    const HGLOBAL dibv5_data = static_cast<HGLOBAL>(
        GetClipboardData(CF_DIBV5));
    const HGLOBAL dib_data = static_cast<HGLOBAL>(
        GetClipboardData(CF_DIB));
    const HBITMAP bitmap_data = static_cast<HBITMAP>(
        GetClipboardData(CF_BITMAP));
    const bool required_formats_readable =
        png_data && GlobalSize(png_data) > 0 &&
        image_png_data && GlobalSize(image_png_data) > 0 &&
        dibv5_data && GlobalSize(dibv5_data) >= sizeof(BITMAPV5HEADER);
    const bool compatibility_formats_readable =
        dib_data && GlobalSize(dib_data) >= sizeof(BITMAPINFOHEADER) &&
        bitmap_data != nullptr;
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
    if (!has_png || !has_image_png ||
        !required_formats_readable ||
        !compatibility_formats_readable) {
        std::cerr << "FAIL: clipboard formats are incomplete; PNG="
                  << has_png << ", image/png=" << has_image_png
                  << ", required readable="
                  << required_formats_readable
                  << ", compatibility readable="
                  << compatibility_formats_readable << '\n';
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
             (dib_visual->bitmap.pixels[0] != 0x33 ||
              dib_visual->bitmap.pixels[1] != 0x66 ||
              dib_visual->bitmap.pixels[2] != 0x99 ||
              dib_visual->bitmap.pixels[3] != 0x80)) ||
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
    std::wstring direct_file_error;
    const auto direct_file_visual =
        airshot::decode_local_image_file(
            file_drop_path,
            &direct_file_error);
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
    if (!direct_file_visual ||
        direct_file_visual->width != source.width ||
        direct_file_visual->height != source.height ||
        direct_file_visual->pixels != source.pixels ||
        !direct_file_error.empty()) {
        std::cerr
            << "FAIL: bounded local image decoder cannot read a valid PNG";
        if (!direct_file_error.empty()) {
            std::cerr << ": " << airshot::to_utf8(direct_file_error);
        }
        std::cerr << '\n';
        return 1;
    }

    const std::filesystem::path missing_path =
        std::filesystem::temp_directory_path() /
        (L"airshot-missing-" +
         std::to_wstring(GetCurrentProcessId()) +
         L".png");
    direct_file_error.clear();
    if (airshot::decode_local_image_file(
            missing_path,
            &direct_file_error) ||
        direct_file_error.empty()) {
        std::cerr
            << "FAIL: missing local image file is not rejected clearly\n";
        return 1;
    }

    direct_file_error.clear();
    if (airshot::decode_local_image_file(
            L"relative-image.png",
            &direct_file_error) ||
        direct_file_error.find(L"绝对路径") == std::wstring::npos) {
        std::cerr
            << "FAIL: relative local image path is not rejected clearly\n";
        return 1;
    }

    direct_file_error.clear();
    if (airshot::decode_local_image_file(
            LR"(\\airshot-invalid-server\share\image.png)",
            &direct_file_error) ||
        direct_file_error.find(L"网络") == std::wstring::npos) {
        std::cerr
            << "FAIL: network image path is not rejected before decoding\n";
        return 1;
    }

    const std::filesystem::path oversized_path =
        std::filesystem::temp_directory_path() /
        (L"airshot-oversized-" +
         std::to_wstring(GetCurrentProcessId()) +
         L".png");
    HANDLE oversized_file = CreateFileW(
        oversized_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    LARGE_INTEGER oversized_length{};
    oversized_length.QuadPart =
        128LL * 1024LL * 1024LL + 1LL;
    const bool oversized_seeded =
        oversized_file != INVALID_HANDLE_VALUE &&
        SetFilePointerEx(
            oversized_file,
            oversized_length,
            nullptr,
            FILE_BEGIN) &&
        SetEndOfFile(oversized_file);
    if (oversized_file != INVALID_HANDLE_VALUE) {
        CloseHandle(oversized_file);
    }
    direct_file_error.clear();
    const bool oversized_rejected =
        oversized_seeded &&
        !airshot::decode_local_image_file(
            oversized_path,
            &direct_file_error) &&
        direct_file_error.find(L"128 MiB") != std::wstring::npos;
    std::filesystem::remove(oversized_path, cleanup_error);
    if (!oversized_rejected) {
        std::cerr
            << "FAIL: oversized local image file is not rejected by byte budget\n";
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
