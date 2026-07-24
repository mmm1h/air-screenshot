#include "airshot/output.h"

#include "airshot/common.h"
#include "airshot/config.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <ole2.h>

#include <array>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>

namespace airshot {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kClipboardBudget = 512ULL * 1024ULL * 1024ULL;

void clear_error(std::wstring* error) {
    if (error) {
        error->clear();
    }
}

void set_error(std::wstring* error, std::wstring message) {
    if (error) {
        *error = std::move(message);
    }
}

[[nodiscard]] bool checked_add(std::size_t first, std::size_t second, std::size_t& result) noexcept {
    if (second > std::numeric_limits<std::size_t>::max() - first) {
        return false;
    }
    result = first + second;
    return true;
}

[[nodiscard]] bool checked_multiply(std::size_t first, std::size_t second, std::size_t& result) noexcept {
    if (first != 0 && second > std::numeric_limits<std::size_t>::max() / first) {
        return false;
    }
    result = first * second;
    return true;
}

class GlobalMemory {
public:
    GlobalMemory() = default;
    explicit GlobalMemory(HGLOBAL value) noexcept : value_(value) {}
    ~GlobalMemory() {
        if (value_) {
            GlobalFree(value_);
        }
    }
    GlobalMemory(const GlobalMemory&) = delete;
    GlobalMemory& operator=(const GlobalMemory&) = delete;
    GlobalMemory(GlobalMemory&& other) noexcept : value_(other.release()) {}
    GlobalMemory& operator=(GlobalMemory&& other) noexcept {
        if (this != &other) {
            if (value_) {
                GlobalFree(value_);
            }
            value_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] HGLOBAL get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
    [[nodiscard]] HGLOBAL release() noexcept {
        const HGLOBAL value = value_;
        value_ = nullptr;
        return value;
    }

private:
    HGLOBAL value_{};
};

class GlobalLockView {
public:
    explicit GlobalLockView(HGLOBAL memory) noexcept : memory_(memory), value_(GlobalLock(memory)) {}
    ~GlobalLockView() {
        if (value_) {
            GlobalUnlock(memory_);
        }
    }
    GlobalLockView(const GlobalLockView&) = delete;
    GlobalLockView& operator=(const GlobalLockView&) = delete;

    [[nodiscard]] void* get() const noexcept { return value_; }

private:
    HGLOBAL memory_{};
    void* value_{};
};

class OwnedBitmap {
public:
    OwnedBitmap() = default;
    explicit OwnedBitmap(HBITMAP value) noexcept : value_(value) {}
    ~OwnedBitmap() {
        if (value_) {
            DeleteObject(value_);
        }
    }
    OwnedBitmap(const OwnedBitmap&) = delete;
    OwnedBitmap& operator=(const OwnedBitmap&) = delete;
    OwnedBitmap(OwnedBitmap&& other) noexcept : value_(other.release()) {}
    OwnedBitmap& operator=(OwnedBitmap&& other) noexcept {
        if (this != &other) {
            if (value_) {
                DeleteObject(value_);
            }
            value_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] HBITMAP get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
    [[nodiscard]] HBITMAP release() noexcept {
        const HBITMAP value = value_;
        value_ = nullptr;
        return value;
    }

private:
    HBITMAP value_{};
};

class ScreenDc {
public:
    ScreenDc() noexcept : value_(GetDC(nullptr)) {}
    ~ScreenDc() {
        if (value_) {
            ReleaseDC(nullptr, value_);
        }
    }
    ScreenDc(const ScreenDc&) = delete;
    ScreenDc& operator=(const ScreenDc&) = delete;

    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HDC value_{};
};

class WinHandle {
public:
    WinHandle() = default;
    explicit WinHandle(HANDLE value) noexcept : value_(value) {}
    ~WinHandle() {
        if (valid()) {
            CloseHandle(value_);
        }
    }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_{};
};

class ClipboardOwner {
public:
    explicit ClipboardOwner(HWND requested) noexcept {
        if (requested && IsWindow(requested)) {
            value_ = requested;
            return;
        }
        created_ = CreateWindowExW(
            0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        value_ = created_;
    }
    ~ClipboardOwner() {
        if (created_) {
            DestroyWindow(created_);
        }
    }
    ClipboardOwner(const ClipboardOwner&) = delete;
    ClipboardOwner& operator=(const ClipboardOwner&) = delete;

    [[nodiscard]] HWND get() const noexcept { return value_; }

private:
    HWND value_{};
    HWND created_{};
};

class TemporaryFile {
public:
    TemporaryFile() = default;
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFile() {
        if (!path_.empty()) {
            DeleteFileW(path_.c_str());
        }
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    void release() noexcept { path_.clear(); }

private:
    std::filesystem::path path_;
};

[[nodiscard]] bool open_clipboard_with_retry(HWND owner, DWORD& last_error) {
    constexpr std::array<DWORD, 7> retry_delays{
        10, 20, 40, 80, 160, 250, 250};
    last_error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 8; ++attempt) {
        SetLastError(ERROR_SUCCESS);
        if (OpenClipboard(owner)) {
            last_error = ERROR_SUCCESS;
            return true;
        }
        last_error = GetLastError();
        if (attempt + 1 < 8) {
            Sleep(retry_delays[static_cast<std::size_t>(attempt)]);
        }
    }
    return false;
}

[[nodiscard]] std::wstring clipboard_open_error_message(DWORD error) {
    if (error == ERROR_SUCCESS) {
        return L"无法打开剪贴板：OpenClipboard 未提供扩展错误。";
    }
    return std::format(
        L"无法打开剪贴板：{} (Win32 {})",
        windows_error_message(error),
        error);
}

[[nodiscard]] std::filesystem::path pictures_directory() {
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &value)) && value) {
        std::filesystem::path result(value);
        CoTaskMemFree(value);
        return result;
    }
    if (value) {
        CoTaskMemFree(value);
    }
    return config_directory();
}

[[nodiscard]] HRESULT create_wic_factory(ComPtr<IWICImagingFactory>& factory) {
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        result =
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    }
    return result;
}

[[nodiscard]] HRESULT encode_png(IWICImagingFactory* factory, IStream* stream, const Bitmap& bitmap) {
    if (!factory || !stream || !bitmap.valid()) {
        return E_INVALIDARG;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    HRESULT result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (SUCCEEDED(result)) {
        result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(result)) {
        result = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
    }
    if (SUCCEEDED(result)) {
        result = frame->Initialize(properties.Get());
    }
    if (SUCCEEDED(result)) {
        result = frame->SetSize(static_cast<UINT>(bitmap.width), static_cast<UINT>(bitmap.height));
    }
    GUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) {
        result = frame->SetPixelFormat(&format);
    }
    if (SUCCEEDED(result) && !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    if (SUCCEEDED(result)) {
        result = frame->WritePixels(static_cast<UINT>(bitmap.height),
                                    static_cast<UINT>(bitmap.stride_bytes()),
                                    static_cast<UINT>(bitmap.pixels.size()),
                                    const_cast<BYTE*>(bitmap.pixels.data()));
    }
    if (SUCCEEDED(result)) {
        result = frame->Commit();
    }
    if (SUCCEEDED(result)) {
        result = encoder->Commit();
    }
    return result;
}

[[nodiscard]] bool make_opaque_copy(const Bitmap& source, Bitmap& result) noexcept {
    if (!source.valid()) {
        return false;
    }
    try {
        result = source;
    } catch (const std::bad_alloc&) {
        result = {};
        return false;
    } catch (const std::length_error&) {
        result = {};
        return false;
    }
    result.make_opaque();
    return result.valid();
}

[[nodiscard]] bool encode_png_bytes(IWICImagingFactory* factory,
                                    const Bitmap& bitmap,
                                    std::vector<std::uint8_t>& bytes,
                                    HRESULT& error) {
    ComPtr<IStream> stream;
    IStream* raw_stream = nullptr;
    error = CreateStreamOnHGlobal(nullptr, TRUE, &raw_stream);
    if (FAILED(error)) {
        return false;
    }
    stream.Attach(raw_stream);

    error = encode_png(factory, stream.Get(), bitmap);
    if (FAILED(error)) {
        return false;
    }

    HGLOBAL stream_memory = nullptr;
    error = GetHGlobalFromStream(stream.Get(), &stream_memory);
    if (FAILED(error) || !stream_memory) {
        return false;
    }

    STATSTG stream_statistics{};
    error = stream->Stat(&stream_statistics, STATFLAG_NONAME);
    if (FAILED(error)) {
        return false;
    }
    const ULONGLONG logical_size = stream_statistics.cbSize.QuadPart;
    if (logical_size == 0 ||
        logical_size > static_cast<ULONGLONG>(std::numeric_limits<std::size_t>::max())) {
        error = E_FAIL;
        return false;
    }
    const auto byte_count = static_cast<std::size_t>(logical_size);
    if (byte_count > GlobalSize(stream_memory)) {
        error = E_FAIL;
        return false;
    }
    GlobalLockView locked(stream_memory);
    if (!locked.get()) {
        error = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    try {
        const auto* begin = static_cast<const std::uint8_t*>(locked.get());
        bytes.assign(begin, begin + byte_count);
    } catch (const std::bad_alloc&) {
        error = E_OUTOFMEMORY;
        return false;
    } catch (const std::length_error&) {
        error = E_OUTOFMEMORY;
        return false;
    }
    return true;
}

[[nodiscard]] GlobalMemory allocate_global(std::size_t size) {
    if (size == 0) {
        return {};
    }
    return GlobalMemory(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, static_cast<SIZE_T>(size)));
}

[[nodiscard]] GlobalMemory global_from_bytes(std::span<const std::uint8_t> bytes) {
    GlobalMemory memory = allocate_global(bytes.size());
    if (!memory) {
        return {};
    }
    GlobalLockView locked(memory.get());
    if (!locked.get()) {
        return {};
    }
    std::memcpy(locked.get(), bytes.data(), bytes.size());
    return memory;
}

[[nodiscard]] GlobalMemory make_dibv5(const Bitmap& bitmap) {
    std::size_t total_size = 0;
    if (!checked_add(sizeof(BITMAPV5HEADER), bitmap.pixels.size(), total_size)) {
        return {};
    }
    GlobalMemory memory = allocate_global(total_size);
    if (!memory) {
        return {};
    }
    GlobalLockView locked(memory.get());
    if (!locked.get()) {
        return {};
    }

    auto* data = static_cast<std::uint8_t*>(locked.get());
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = bitmap.width;
    header.bV5Height = bitmap.height;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = static_cast<DWORD>(bitmap.pixels.size());
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = LCS_sRGB;
    std::memcpy(data, &header, sizeof(header));

    auto* destination = data + sizeof(header);
    for (int row = bitmap.height - 1; row >= 0; --row) {
        const auto source_row = bitmap.row(row);
        auto* destination_row =
            destination + static_cast<std::size_t>(bitmap.height - 1 - row) * bitmap.stride_bytes();
        std::memcpy(destination_row, source_row.data(), bitmap.stride_bytes());
    }
    return memory;
}

[[nodiscard]] std::optional<std::size_t> dib24_size(const Bitmap& bitmap,
                                                    std::size_t& stride,
                                                    std::size_t& image_size) noexcept {
    std::size_t raw_stride = 0;
    if (!checked_multiply(static_cast<std::size_t>(bitmap.width), 3, raw_stride) ||
        raw_stride > std::numeric_limits<std::size_t>::max() - 3) {
        return std::nullopt;
    }
    stride = (raw_stride + 3) & ~std::size_t{3};
    if (!checked_multiply(stride, static_cast<std::size_t>(bitmap.height), image_size) ||
        image_size > std::numeric_limits<DWORD>::max()) {
        return std::nullopt;
    }
    std::size_t total_size = 0;
    if (!checked_add(sizeof(BITMAPINFOHEADER), image_size, total_size)) {
        return std::nullopt;
    }
    return total_size;
}

[[nodiscard]] GlobalMemory make_dib24(const Bitmap& bitmap) {
    std::size_t stride = 0;
    std::size_t image_size = 0;
    const auto total_size = dib24_size(bitmap, stride, image_size);
    if (!total_size) {
        return {};
    }
    GlobalMemory memory = allocate_global(*total_size);
    if (!memory) {
        return {};
    }
    GlobalLockView locked(memory.get());
    if (!locked.get()) {
        return {};
    }

    auto* data = static_cast<std::uint8_t*>(locked.get());
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = bitmap.width;
    header.biHeight = bitmap.height;
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(image_size);
    std::memcpy(data, &header, sizeof(header));

    auto* destination = data + sizeof(header);
    for (int row = bitmap.height - 1; row >= 0; --row) {
        const auto source_row = bitmap.row(row);
        auto* destination_row = destination + static_cast<std::size_t>(bitmap.height - 1 - row) * stride;
        for (int column = 0; column < bitmap.width; ++column) {
            const std::size_t source_index = static_cast<std::size_t>(column) * Bitmap::bytes_per_pixel;
            const std::size_t destination_index = static_cast<std::size_t>(column) * 3;
            std::memcpy(destination_row + destination_index, source_row.data() + source_index, 3);
        }
    }
    return memory;
}

[[nodiscard]] OwnedBitmap make_device_bitmap(const Bitmap& bitmap) {
    ScreenDc screen;
    if (!screen.get()) {
        return {};
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap.width;
    info.bmiHeader.biHeight = bitmap.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(bitmap.pixels.size());
    void* bits = nullptr;
    OwnedBitmap result(CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!result || !bits) {
        return {};
    }
    auto* destination = static_cast<std::uint8_t*>(bits);
    for (int row = bitmap.height - 1; row >= 0; --row) {
        const auto source_row = bitmap.row(row);
        auto* destination_row =
            destination + static_cast<std::size_t>(bitmap.height - 1 - row) * bitmap.stride_bytes();
        std::memcpy(destination_row, source_row.data(), bitmap.stride_bytes());
    }
    return result;
}

[[nodiscard]] bool publish_global(UINT format, GlobalMemory& memory) {
    if (!memory || !SetClipboardData(format, memory.get())) {
        return false;
    }
    (void)memory.release();
    return true;
}

[[nodiscard]] bool publish_bitmap(OwnedBitmap& bitmap) {
    if (!bitmap || !SetClipboardData(CF_BITMAP, bitmap.get())) {
        return false;
    }
    (void)bitmap.release();
    return true;
}

[[nodiscard]] std::optional<std::filesystem::path> reserve_temporary_file(
    const std::filesystem::path& directory,
    DWORD& error) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) {
            error = ERROR_GEN_FAILURE;
            return std::nullopt;
        }
        wchar_t guid_text[40]{};
        if (StringFromGUID2(guid, guid_text, static_cast<int>(std::size(guid_text))) <= 0) {
            error = ERROR_GEN_FAILURE;
            return std::nullopt;
        }
        const auto candidate = directory / std::format(L".airshot-{}.tmp", guid_text);
        WinHandle handle(CreateFileW(candidate.c_str(),
                                     GENERIC_READ | GENERIC_WRITE,
                                     0,
                                     nullptr,
                                     CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL,
                                     nullptr));
        if (handle.valid()) {
            error = ERROR_SUCCESS;
            return candidate;
        }
        error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return std::nullopt;
        }
    }
    error = ERROR_FILE_EXISTS;
    return std::nullopt;
}

[[nodiscard]] HRESULT encode_png_file(IWICImagingFactory* factory,
                                      const Bitmap& bitmap,
                                      const std::filesystem::path& path) {
    ComPtr<IWICStream> stream;
    HRESULT result = factory->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(result)) {
        result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    }
    if (SUCCEEDED(result)) {
        result = encode_png(factory, stream.Get(), bitmap);
    }
    return result;
}

[[nodiscard]] bool flush_file_for_commit(const std::filesystem::path& path, DWORD& error) {
    WinHandle handle(CreateFileW(path.c_str(),
                                 GENERIC_WRITE,
                                 0,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                 nullptr));
    if (!handle.valid()) {
        error = GetLastError();
        return false;
    }
    if (!FlushFileBuffers(handle.get())) {
        error = GetLastError();
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

[[nodiscard]] bool replace_file_atomically(const std::filesystem::path& temporary,
                                           const std::filesystem::path& destination,
                                           DWORD& error) {
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            error = ERROR_ACCESS_DENIED;
            return false;
        }
        if (!ReplaceFileW(destination.c_str(),
                          temporary.c_str(),
                          nullptr,
                          0,
                          nullptr,
                          nullptr)) {
            error = GetLastError();
            return false;
        }
    } else {
        const DWORD attributes_error = GetLastError();
        if (attributes_error != ERROR_FILE_NOT_FOUND && attributes_error != ERROR_PATH_NOT_FOUND) {
            error = attributes_error;
            return false;
        }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error = GetLastError();
            return false;
        }
    }
    error = ERROR_SUCCESS;
    return true;
}

[[nodiscard]] bool has_png_extension(const std::filesystem::path& path) {
    const std::wstring extension = path.extension().wstring();
    return _wcsicmp(extension.c_str(), L".png") == 0;
}

[[nodiscard]] bool is_reserved_device_filename(
    const std::filesystem::path& path) {
    std::wstring name = path.filename().wstring();
    while (!name.empty() &&
           (name.back() == L' ' || name.back() == L'.')) {
        name.pop_back();
    }
    const std::size_t extension = name.find(L'.');
    if (extension != std::wstring::npos) {
        name.resize(extension);
    }
    while (!name.empty() && name.back() == L' ') {
        name.pop_back();
    }
    std::ranges::transform(
        name,
        name.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(
                std::towupper(character));
        });
    if (name == L"CON" || name == L"PRN" ||
        name == L"AUX" || name == L"NUL" ||
        name == L"CLOCK$" || name == L"CONIN$" ||
        name == L"CONOUT$") {
        return true;
    }
    return name.size() == 4U &&
           (name.starts_with(L"COM") ||
            name.starts_with(L"LPT")) &&
           name.back() >= L'1' && name.back() <= L'9';
}

[[nodiscard]] std::filesystem::path unique_generated_path(const std::filesystem::path& directory) {
    const std::wstring stem = std::format(L"AirShot-{}", timestamp_for_file());
    GUID guid{};
    wchar_t guid_text[40]{};
    if (SUCCEEDED(CoCreateGuid(&guid)) &&
        StringFromGUID2(guid, guid_text, static_cast<int>(std::size(guid_text))) > 2) {
        const std::wstring_view value(guid_text);
        return directory /
               std::format(L"{}-{}.png", stem, value.substr(1, value.size() - 2));
    }

    static std::atomic_uint64_t fallback_sequence{};
    return directory /
           std::format(L"{}-{}-{}-{}.png",
                       stem,
                       GetCurrentProcessId(),
                       GetTickCount64(),
                       fallback_sequence.fetch_add(1, std::memory_order_relaxed));
}

}  // namespace

bool copy_bitmap_to_clipboard(HWND owner, const Bitmap& bitmap, std::wstring* error) {
    clear_error(error);
    if (!bitmap.valid()) {
        set_error(error, L"图像为空或像素缓冲区无效。");
        return false;
    }

    std::size_t dibv5_bytes = 0;
    if (!checked_add(sizeof(BITMAPV5HEADER), bitmap.pixels.size(), dibv5_bytes) ||
        dibv5_bytes > kClipboardBudget) {
        set_error(error, L"图像过大，超过 512 MiB 剪贴板预算。");
        return false;
    }

    const ScopedWinrtApartment apartment(true);
    if (!apartment.available()) {
        set_error(error, L"无法初始化图像编码组件。");
        return false;
    }

    Bitmap opaque_bitmap;
    if (!make_opaque_copy(bitmap, opaque_bitmap)) {
        set_error(error, L"无法为剪贴板分配图像内存。");
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT encoding_error = create_wic_factory(factory);
    if (FAILED(encoding_error)) {
        set_error(error, windows_error_message(static_cast<DWORD>(encoding_error)));
        return false;
    }
    std::vector<std::uint8_t> png_bytes;
    if (!encode_png_bytes(factory.Get(), opaque_bitmap, png_bytes, encoding_error)) {
        set_error(error, windows_error_message(static_cast<DWORD>(encoding_error)));
        return false;
    }

    std::size_t png_pair_bytes = 0;
    std::size_t mandatory_bytes = 0;
    if (!checked_multiply(png_bytes.size(), 2, png_pair_bytes) ||
        !checked_add(dibv5_bytes, png_pair_bytes, mandatory_bytes) ||
        mandatory_bytes > kClipboardBudget) {
        set_error(error, L"图像过大，超过 512 MiB 剪贴板预算。");
        return false;
    }

    const UINT png_format = RegisterClipboardFormatW(L"PNG");
    const UINT png_format_alt = RegisterClipboardFormatW(L"image/png");
    if (png_format == 0 || png_format_alt == 0) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }

    GlobalMemory png_memory = global_from_bytes(png_bytes);
    GlobalMemory png_memory_alt = global_from_bytes(png_bytes);
    GlobalMemory dibv5_memory = make_dibv5(opaque_bitmap);
    if (!png_memory || !png_memory_alt || !dibv5_memory) {
        set_error(error, L"无法为剪贴板分配图像内存。");
        return false;
    }

    GlobalMemory dib_memory;
    OwnedBitmap device_bitmap;
    std::size_t dib_stride = 0;
    std::size_t dib_image_bytes = 0;
    const auto optional_dib_bytes = dib24_size(opaque_bitmap, dib_stride, dib_image_bytes);
    std::size_t optional_total = 0;
    std::size_t with_dib = 0;
    if (optional_dib_bytes &&
        checked_add(mandatory_bytes, *optional_dib_bytes, with_dib) &&
        checked_add(with_dib, opaque_bitmap.pixels.size(), optional_total) &&
        optional_total <= kClipboardBudget) {
        dib_memory = make_dib24(opaque_bitmap);
        device_bitmap = make_device_bitmap(opaque_bitmap);
        if (!dib_memory || !device_bitmap) {
            dib_memory = {};
            device_bitmap = {};
        }
    }

    ClipboardOwner clipboard_owner(owner);
    if (!clipboard_owner.get()) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    DWORD clipboard_error = ERROR_SUCCESS;
    if (!open_clipboard_with_retry(clipboard_owner.get(), clipboard_error)) {
        set_error(error, clipboard_open_error_message(clipboard_error));
        return false;
    }
    if (!EmptyClipboard()) {
        const DWORD last_error = GetLastError();
        CloseClipboard();
        set_error(error, windows_error_message(last_error));
        return false;
    }

    if (!publish_global(png_format, png_memory) ||
        !publish_global(png_format_alt, png_memory_alt) ||
        !publish_global(CF_DIBV5, dibv5_memory)) {
        const DWORD last_error = GetLastError();
        EmptyClipboard();
        CloseClipboard();
        set_error(error, windows_error_message(last_error));
        return false;
    }

    if (dib_memory && device_bitmap) {
        (void)publish_global(CF_DIB, dib_memory);
        (void)publish_bitmap(device_bitmap);
    }

    if (!CloseClipboard()) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    return true;
}

bool copy_bitmap_to_clipboard(const Bitmap& bitmap, std::wstring* error) {
    return copy_bitmap_to_clipboard(nullptr, bitmap, error);
}

bool copy_text_to_clipboard(HWND owner, std::wstring_view text, std::wstring* error) {
    clear_error(error);
    if (text.size() > std::numeric_limits<std::size_t>::max() / sizeof(wchar_t) - 1) {
        set_error(error, L"文本过大，无法复制到剪贴板。");
        return false;
    }

    const std::size_t bytes = (text.size() + 1U) * sizeof(wchar_t);
    GlobalMemory memory = allocate_global(bytes);
    if (!memory) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    {
        GlobalLockView locked(memory.get());
        if (!locked.get()) {
            set_error(error, windows_error_message(GetLastError()));
            return false;
        }
        if (!text.empty()) {
            std::memcpy(locked.get(), text.data(), text.size() * sizeof(wchar_t));
        }
    }

    ClipboardOwner clipboard_owner(owner);
    if (!clipboard_owner.get()) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    DWORD clipboard_error = ERROR_SUCCESS;
    if (!open_clipboard_with_retry(clipboard_owner.get(), clipboard_error)) {
        set_error(error, clipboard_open_error_message(clipboard_error));
        return false;
    }
    if (!EmptyClipboard()) {
        const DWORD last_error = GetLastError();
        CloseClipboard();
        set_error(error, windows_error_message(last_error));
        return false;
    }
    if (!publish_global(CF_UNICODETEXT, memory)) {
        const DWORD last_error = GetLastError();
        EmptyClipboard();
        CloseClipboard();
        set_error(error, windows_error_message(last_error));
        return false;
    }
    if (!CloseClipboard()) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    return true;
}

bool copy_text_to_clipboard(std::wstring_view text, std::wstring* error) {
    return copy_text_to_clipboard(nullptr, text, error);
}

bool save_png(const Bitmap& bitmap, const std::filesystem::path& path, std::wstring* error) {
    clear_error(error);
    if (!bitmap.valid()) {
        set_error(error, L"图像为空或像素缓冲区无效。");
        return false;
    }
    if (path.empty() || path.filename().empty()) {
        set_error(error, L"输出路径无效。");
        return false;
    }
    if (!has_png_extension(path)) {
        set_error(error, L"输出文件必须使用 .png 扩展名。");
        return false;
    }
    if (is_reserved_device_filename(path)) {
        set_error(error, L"输出文件名不能使用 Windows 保留设备名。");
        return false;
    }

    const std::filesystem::path directory = path.parent_path();
    if (!directory.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(directory, directory_error);
        if (directory_error) {
            set_error(error, windows_error_message(static_cast<DWORD>(directory_error.value())));
            return false;
        }
    }

    DWORD temporary_error = ERROR_SUCCESS;
    const auto temporary_path = reserve_temporary_file(directory, temporary_error);
    if (!temporary_path) {
        set_error(error, windows_error_message(temporary_error));
        return false;
    }
    TemporaryFile temporary(*temporary_path);

    const ScopedWinrtApartment apartment(true);
    if (!apartment.available()) {
        set_error(error, L"无法初始化图像编码组件。");
        return false;
    }
    Bitmap opaque_bitmap;
    if (!make_opaque_copy(bitmap, opaque_bitmap)) {
        set_error(error, L"无法为 PNG 编码分配图像内存。");
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = create_wic_factory(factory);
    if (SUCCEEDED(result)) {
        result = encode_png_file(factory.Get(), opaque_bitmap, temporary.path());
    }
    factory.Reset();
    if (FAILED(result)) {
        set_error(error, windows_error_message(static_cast<DWORD>(result)));
        return false;
    }

    DWORD file_error = ERROR_SUCCESS;
    if (!flush_file_for_commit(temporary.path(), file_error)) {
        set_error(error, windows_error_message(file_error));
        return false;
    }
    if (!replace_file_atomically(temporary.path(), path, file_error)) {
        set_error(error, windows_error_message(file_error));
        return false;
    }
    temporary.release();
    return true;
}

std::filesystem::path resolve_output_path(std::wstring_view requested) {
    if (requested.empty()) {
        return unique_generated_path(pictures_directory() / L"Air Screenshot");
    }

    const bool trailing_separator = requested.back() == L'\\' || requested.back() == L'/';
    std::filesystem::path path(requested);
    std::error_code code;
    if (trailing_separator || std::filesystem::is_directory(path, code)) {
        return unique_generated_path(path);
    }
    if (!path.has_extension()) {
        path += L".png";
    }
    return path;
}

std::optional<std::filesystem::path> prompt_png_path(HWND owner) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) {
        return std::nullopt;
    }
    const COMDLG_FILTERSPEC filter[] = {{L"PNG 图像", L"*.png"}};
    dialog->SetFileTypes(1, filter);
    dialog->SetDefaultExtension(L"png");
    const std::wstring name = std::format(L"AirShot-{}.png", timestamp_for_file());
    dialog->SetFileName(name.c_str());
    if (FAILED(dialog->Show(owner))) {
        return std::nullopt;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) {
        return std::nullopt;
    }
    PWSTR value = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)) || !value) {
        return std::nullopt;
    }
    std::filesystem::path result(value);
    CoTaskMemFree(value);
    return result;
}

}  // namespace airshot
