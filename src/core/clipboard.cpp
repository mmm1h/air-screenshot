#include "airshot/clipboard.h"

#include "airshot/common.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <limits>
#include <new>
#include <ranges>
#include <string>

namespace airshot {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kClipboardPayloadBudget =
    128ULL * 1024ULL * 1024ULL;
constexpr std::size_t kDecodedVisualBudget =
    64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumTextCharacters = 64ULL * 1024ULL;
constexpr DWORD kBiAlphaBitfields = 6;

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

class ClipboardOwner {
public:
    explicit ClipboardOwner(HWND requested) noexcept {
        if (requested && IsWindow(requested)) {
            value_ = requested;
            return;
        }
        created_ = CreateWindowExW(
            0,
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
            nullptr);
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

class ClipboardScope {
public:
    explicit ClipboardScope(HWND owner) noexcept {
        constexpr std::array<DWORD, 7> delays{
            10, 20, 40, 80, 160, 250, 250};
        for (std::size_t attempt = 0; attempt <= delays.size(); ++attempt) {
            SetLastError(ERROR_SUCCESS);
            if (OpenClipboard(owner)) {
                open_ = true;
                error_ = ERROR_SUCCESS;
                return;
            }
            error_ = GetLastError();
            if (attempt < delays.size()) {
                Sleep(delays[attempt]);
            }
        }
    }

    ~ClipboardScope() {
        if (open_) {
            CloseClipboard();
        }
    }

    ClipboardScope(const ClipboardScope&) = delete;
    ClipboardScope& operator=(const ClipboardScope&) = delete;

    [[nodiscard]] bool open() const noexcept { return open_; }
    [[nodiscard]] DWORD error() const noexcept { return error_; }

private:
    bool open_{};
    DWORD error_{ERROR_SUCCESS};
};

class GlobalLockView {
public:
    explicit GlobalLockView(HGLOBAL memory) noexcept
        : memory_(memory), value_(memory ? GlobalLock(memory) : nullptr) {}

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

class MemoryDc {
public:
    explicit MemoryDc(HDC compatible) noexcept
        : value_(CreateCompatibleDC(compatible)) {}
    ~MemoryDc() {
        if (value_) {
            DeleteDC(value_);
        }
    }
    MemoryDc(const MemoryDc&) = delete;
    MemoryDc& operator=(const MemoryDc&) = delete;
    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HDC value_{};
};

class OwnedGdiObject {
public:
    explicit OwnedGdiObject(HGDIOBJ value = nullptr) noexcept : value_(value) {}
    ~OwnedGdiObject() {
        if (value_) {
            DeleteObject(value_);
        }
    }
    OwnedGdiObject(const OwnedGdiObject&) = delete;
    OwnedGdiObject& operator=(const OwnedGdiObject&) = delete;
    [[nodiscard]] HGDIOBJ get() const noexcept { return value_; }

private:
    HGDIOBJ value_{};
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
    OwnedBitmap(OwnedBitmap&& other) noexcept
        : value_(other.release()) {}
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
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }
    [[nodiscard]] HBITMAP release() noexcept {
        const HBITMAP value = value_;
        value_ = nullptr;
        return value;
    }

private:
    HBITMAP value_{};
};

class SelectedObject {
public:
    SelectedObject(HDC dc, HGDIOBJ value) noexcept
        : dc_(dc), previous_(dc && value ? SelectObject(dc, value) : nullptr) {}
    ~SelectedObject() {
        if (valid()) {
            SelectObject(dc_, previous_);
        }
    }
    SelectedObject(const SelectedObject&) = delete;
    SelectedObject& operator=(const SelectedObject&) = delete;
    [[nodiscard]] bool valid() const noexcept {
        return previous_ && previous_ != HGDI_ERROR;
    }

private:
    HDC dc_{};
    HGDIOBJ previous_{};
};

[[nodiscard]] HRESULT create_wic_factory(
    ComPtr<IWICImagingFactory>& factory) {
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        result = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()));
    }
    return result;
}

[[nodiscard]] std::optional<Bitmap> decode_wic_source(
    IWICImagingFactory* factory,
    IWICBitmapSource* source) {
    if (!factory || !source) {
        return std::nullopt;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(source->GetSize(&width, &height)) ||
        width == 0 || height == 0 ||
        width > static_cast<UINT>(std::numeric_limits<int>::max()) ||
        height > static_cast<UINT>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const auto bytes = Bitmap::checked_byte_size(
        static_cast<int>(width),
        static_cast<int>(height));
    if (!bytes || *bytes > kDecodedVisualBudget ||
        *bytes > std::numeric_limits<UINT>::max()) {
        return std::nullopt;
    }

    ComPtr<IWICFormatConverter> converter;
    HRESULT result =
        factory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(result)) {
        result = converter->Initialize(
            source,
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
    }
    if (FAILED(result)) {
        return std::nullopt;
    }

    try {
        Bitmap bitmap(static_cast<int>(width), static_cast<int>(height));
        result = converter->CopyPixels(
            nullptr,
            static_cast<UINT>(bitmap.stride_bytes()),
            static_cast<UINT>(bitmap.pixels.size()),
            bitmap.pixels.data());
        if (FAILED(result)) {
            return std::nullopt;
        }
        return bitmap;
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<Bitmap> decode_wic_stream(
    IWICImagingFactory* factory,
    IStream* stream) {
    if (!factory || !stream) {
        return std::nullopt;
    }
    LARGE_INTEGER start{};
    if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) {
        return std::nullopt;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT result = factory->CreateDecoderFromStream(
        stream,
        nullptr,
        WICDecodeMetadataCacheOnLoad,
        decoder.GetAddressOf());
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(result)) {
        result = decoder->GetFrame(0, frame.GetAddressOf());
    }
    if (FAILED(result)) {
        return std::nullopt;
    }
    return decode_wic_source(factory, frame.Get());
}

[[nodiscard]] std::optional<Bitmap> decode_image_bytes(
    IWICImagingFactory* factory,
    std::span<const std::uint8_t> bytes) {
    if (!factory || bytes.empty() ||
        bytes.size() > kClipboardPayloadBudget) {
        return std::nullopt;
    }
    HGLOBAL memory = GlobalAlloc(
        GMEM_MOVEABLE,
        static_cast<SIZE_T>(bytes.size()));
    if (!memory) {
        return std::nullopt;
    }
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return std::nullopt;
    }
    std::memcpy(destination, bytes.data(), bytes.size());
    GlobalUnlock(memory);

    ComPtr<IStream> stream;
    IStream* raw_stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &raw_stream)) ||
        !raw_stream) {
        GlobalFree(memory);
        return std::nullopt;
    }
    stream.Attach(raw_stream);
    return decode_wic_stream(factory, stream.Get());
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
copy_global_bytes(
    HGLOBAL memory,
    std::size_t maximum_bytes = kClipboardPayloadBudget) {
    if (!memory) {
        return std::nullopt;
    }
    const SIZE_T size = GlobalSize(memory);
    if (size == 0 ||
        size > kClipboardPayloadBudget ||
        size > maximum_bytes) {
        return std::nullopt;
    }
    GlobalLockView locked(memory);
    if (!locked.get()) {
        return std::nullopt;
    }
    try {
        const auto* begin =
            static_cast<const std::uint8_t*>(locked.get());
        return std::vector<std::uint8_t>(begin, begin + size);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<Bitmap> decode_dib_bytes(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < sizeof(BITMAPINFOHEADER)) {
        return std::nullopt;
    }

    DWORD header_size = 0;
    std::memcpy(&header_size, bytes.data(), sizeof(header_size));
    if (header_size < sizeof(BITMAPINFOHEADER) ||
        header_size > bytes.size()) {
        return std::nullopt;
    }

    BITMAPINFOHEADER header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.biWidth <= 0 || header.biHeight == 0 ||
        header.biHeight == std::numeric_limits<LONG>::min() ||
        header.biPlanes != 1 ||
        (header.biBitCount != 1 &&
         header.biBitCount != 4 &&
         header.biBitCount != 8 &&
         header.biBitCount != 16 &&
         header.biBitCount != 24 &&
         header.biBitCount != 32) ||
        (header.biCompression != BI_RGB &&
         header.biCompression != BI_BITFIELDS &&
         header.biCompression != kBiAlphaBitfields)) {
        return std::nullopt;
    }

    const int width = header.biWidth;
    const int height = std::abs(header.biHeight);
    const auto decoded_size =
        Bitmap::checked_byte_size(width, height);
    if (!decoded_size ||
        *decoded_size > kDecodedVisualBudget) {
        return std::nullopt;
    }

    std::size_t pixel_offset = header_size;
    if (header_size == sizeof(BITMAPINFOHEADER) &&
        (header.biCompression == BI_BITFIELDS ||
         header.biCompression == kBiAlphaBitfields)) {
        const std::size_t mask_count =
            header.biCompression == kBiAlphaBitfields ? 4U : 3U;
        if (mask_count >
            (std::numeric_limits<std::size_t>::max() - pixel_offset) /
                sizeof(DWORD)) {
            return std::nullopt;
        }
        pixel_offset += mask_count * sizeof(DWORD);
    }
    const std::size_t palette_entries =
        header.biClrUsed != 0
            ? static_cast<std::size_t>(header.biClrUsed)
            : (header.biBitCount <= 8
                   ? std::size_t{1} << header.biBitCount
                   : 0U);
    if (palette_entries >
        (std::numeric_limits<std::size_t>::max() - pixel_offset) /
            sizeof(RGBQUAD)) {
        return std::nullopt;
    }
    pixel_offset += palette_entries * sizeof(RGBQUAD);

    const std::uint64_t row_bits =
        static_cast<std::uint64_t>(width) * header.biBitCount;
    const std::uint64_t source_stride =
        ((row_bits + 31U) / 32U) * 4U;
    const std::uint64_t source_bytes =
        source_stride * static_cast<std::uint64_t>(height);
    if (pixel_offset > bytes.size() ||
        source_bytes >
            static_cast<std::uint64_t>(
                bytes.size() - pixel_offset)) {
        return std::nullopt;
    }

    ScreenDc screen;
    MemoryDc memory(screen.get());
    if (!screen.get() || !memory.get()) {
        return std::nullopt;
    }
    BITMAPINFO destination_info{};
    destination_info.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);
    destination_info.bmiHeader.biWidth = width;
    destination_info.bmiHeader.biHeight = -height;
    destination_info.bmiHeader.biPlanes = 1;
    destination_info.bmiHeader.biBitCount = 32;
    destination_info.bmiHeader.biCompression = BI_RGB;
    void* destination_bits = nullptr;
    OwnedGdiObject destination_bitmap(CreateDIBSection(
        screen.get(),
        &destination_info,
        DIB_RGB_COLORS,
        &destination_bits,
        nullptr,
        0));
    if (!destination_bitmap.get() || !destination_bits) {
        return std::nullopt;
    }
    SelectedObject selected(
        memory.get(),
        destination_bitmap.get());
    if (!selected.valid()) {
        return std::nullopt;
    }
    const int copied = StretchDIBits(
        memory.get(),
        0,
        0,
        width,
        height,
        0,
        0,
        width,
        height,
        bytes.data() + pixel_offset,
        reinterpret_cast<const BITMAPINFO*>(bytes.data()),
        DIB_RGB_COLORS,
        SRCCOPY);
    if (copied == 0 || copied == GDI_ERROR) {
        return std::nullopt;
    }

    try {
        Bitmap bitmap(width, height);
        std::memcpy(
            bitmap.pixels.data(),
            destination_bits,
            bitmap.pixels.size());
        bool declared_alpha =
            header.biCompression == kBiAlphaBitfields;
        if (!declared_alpha &&
            header_size >= sizeof(BITMAPV4HEADER)) {
            BITMAPV4HEADER v4{};
            std::memcpy(&v4, bytes.data(), sizeof(v4));
            declared_alpha = v4.bV4AlphaMask != 0;
        }
        if (!declared_alpha) {
            bitmap.make_opaque();
        }
        return bitmap;
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<Bitmap> decode_image_file(
    IWICImagingFactory* factory,
    const std::filesystem::path& path) {
    if (!factory || path.empty()) {
        return std::nullopt;
    }
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(path, file_error) ||
        file_error) {
        return std::nullopt;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT result = factory->CreateDecoderFromFilename(
        path.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        decoder.GetAddressOf());
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(result)) {
        result = decoder->GetFrame(0, frame.GetAddressOf());
    }
    if (FAILED(result)) {
        return std::nullopt;
    }
    return decode_wic_source(factory, frame.Get());
}

[[nodiscard]] std::optional<Bitmap> decode_hbitmap(
    IWICImagingFactory* factory,
    HBITMAP handle) {
    if (!factory || !handle) {
        return std::nullopt;
    }
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromHBITMAP(
            handle,
            nullptr,
            WICBitmapIgnoreAlpha,
            bitmap.GetAddressOf()))) {
        return std::nullopt;
    }
    return decode_wic_source(factory, bitmap.Get());
}

[[nodiscard]] std::wstring trim(std::wstring_view value) {
    const auto is_space = [](wchar_t character) {
        return std::iswspace(character) != 0;
    };
    auto first = std::ranges::find_if_not(value, is_space);
    auto last = std::ranges::find_if_not(value | std::views::reverse, is_space).base();
    if (first >= last) {
        return {};
    }
    return std::wstring(first, last);
}

[[nodiscard]] std::optional<int> parse_decimal_component(
    std::wstring_view value) {
    const std::wstring part = trim(value);
    if (part.empty() || part.size() > 3) {
        return std::nullopt;
    }
    int component = 0;
    for (const wchar_t character : part) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        component = component * 10 + static_cast<int>(character - L'0');
    }
    if (component > 255) {
        return std::nullopt;
    }
    return component;
}

[[nodiscard]] std::optional<COLORREF> parse_color(
    std::wstring_view value) {
    const std::wstring normalized = trim(value);
    if (normalized.size() == 4 && normalized[0] == L'#') {
        auto hex = [](wchar_t character) -> std::optional<int> {
            if (character >= L'0' && character <= L'9') {
                return static_cast<int>(character - L'0');
            }
            const wchar_t upper =
                static_cast<wchar_t>(std::towupper(character));
            if (upper >= L'A' && upper <= L'F') {
                return 10 + static_cast<int>(upper - L'A');
            }
            return std::nullopt;
        };
        const auto red = hex(normalized[1]);
        const auto green = hex(normalized[2]);
        const auto blue = hex(normalized[3]);
        if (red && green && blue) {
            return RGB(*red * 17, *green * 17, *blue * 17);
        }
    }
    if (normalized.size() == 7 && normalized[0] == L'#') {
        unsigned int rgb = 0;
        for (std::size_t index = 1; index < normalized.size(); ++index) {
            const wchar_t character =
                static_cast<wchar_t>(std::towupper(normalized[index]));
            const int digit =
                character >= L'0' && character <= L'9'
                    ? static_cast<int>(character - L'0')
                    : (character >= L'A' && character <= L'F'
                           ? 10 + static_cast<int>(character - L'A')
                           : -1);
            if (digit < 0) {
                return std::nullopt;
            }
            rgb = (rgb << 4U) | static_cast<unsigned int>(digit);
        }
        return RGB((rgb >> 16U) & 0xffU,
                   (rgb >> 8U) & 0xffU,
                   rgb & 0xffU);
    }

    std::wstring lower_value = normalized;
    std::ranges::transform(
        lower_value,
        lower_value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    if (!lower_value.starts_with(L"rgb(") ||
        !lower_value.ends_with(L')')) {
        return std::nullopt;
    }
    const std::wstring_view components(
        lower_value.data() + 4,
        lower_value.size() - 5);
    const std::size_t first_comma = components.find(L',');
    const std::size_t second_comma =
        first_comma == std::wstring_view::npos
            ? std::wstring_view::npos
            : components.find(L',', first_comma + 1);
    if (first_comma == std::wstring_view::npos ||
        second_comma == std::wstring_view::npos ||
        components.find(L',', second_comma + 1) != std::wstring_view::npos) {
        return std::nullopt;
    }
    const auto red = parse_decimal_component(
        components.substr(0, first_comma));
    const auto green = parse_decimal_component(
        components.substr(
            first_comma + 1,
            second_comma - first_comma - 1));
    const auto blue = parse_decimal_component(
        components.substr(second_comma + 1));
    if (!red || !green || !blue) {
        return std::nullopt;
    }
    return RGB(*red, *green, *blue);
}

[[nodiscard]] std::wstring canonical_color(COLORREF color) {
    return std::format(
        L"#{:02X}{:02X}{:02X}",
        GetRValue(color),
        GetGValue(color),
        GetBValue(color));
}

[[nodiscard]] std::optional<Bitmap> render_color_card(
    COLORREF color,
    std::wstring_view label) {
    constexpr int width = 320;
    constexpr int height = 200;
    try {
        Bitmap bitmap(width, height);
        for (int y = 0; y < height; ++y) {
            auto row = bitmap.row(y);
            for (int x = 0; x < width; ++x) {
                const std::size_t offset =
                    static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel;
                row[offset] = GetBValue(color);
                row[offset + 1] = GetGValue(color);
                row[offset + 2] = GetRValue(color);
                row[offset + 3] = 255;
            }
        }

        ScreenDc screen;
        MemoryDc memory(screen.get());
        if (!screen.get() || !memory.get()) {
            return bitmap;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        OwnedGdiObject native(CreateDIBSection(
            screen.get(),
            &info,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0));
        if (!native.get() || !bits) {
            return bitmap;
        }
        std::memcpy(bits, bitmap.pixels.data(), bitmap.pixels.size());
        SelectedObject selected(memory.get(), native.get());
        if (!selected.valid()) {
            return bitmap;
        }

        OwnedGdiObject font(CreateFontW(
            -24,
            0,
            0,
            0,
            FW_SEMIBOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"));
        SelectedObject selected_font(memory.get(), font.get());
        SetBkMode(memory.get(), TRANSPARENT);
        const int luminance =
            (299 * GetRValue(color) +
             587 * GetGValue(color) +
             114 * GetBValue(color)) /
            1000;
        SetTextColor(
            memory.get(),
            luminance >= 150 ? RGB(20, 25, 31) : RGB(255, 255, 255));
        RECT text_rect{20, 20, width - 20, height - 20};
        DrawTextW(
            memory.get(),
            label.data(),
            static_cast<int>(label.size()),
            &text_rect,
            DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        std::memcpy(bitmap.pixels.data(), bits, bitmap.pixels.size());
        bitmap.make_opaque();
        return bitmap;
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<Bitmap> render_text_card(
    std::wstring_view original) {
    std::wstring text(original.substr(
        0,
        std::min(original.size(), kMaximumTextCharacters)));
    if (text.empty()) {
        return std::nullopt;
    }

    ScreenDc screen;
    MemoryDc measure(screen.get());
    if (!screen.get() || !measure.get()) {
        return std::nullopt;
    }
    OwnedGdiObject font(CreateFontW(
        -20,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"));
    if (!font.get()) {
        return std::nullopt;
    }
    SelectedObject selected_measure_font(measure.get(), font.get());
    constexpr int margin = 24;
    constexpr int maximum_content_width = 672;
    RECT measured{0, 0, maximum_content_width, 0};
    const UINT flags =
        DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL |
        DT_NOPREFIX | DT_EXPANDTABS;
    if (DrawTextW(
            measure.get(),
            text.c_str(),
            static_cast<int>(text.size()),
            &measured,
            flags) == 0 &&
        !text.empty()) {
        measured = {0, 0, 240, 28};
    }
    const int width =
        std::clamp(
            static_cast<int>(measured.right - measured.left) +
                margin * 2,
            280,
            720);
    const int height =
        std::clamp(
            static_cast<int>(measured.bottom - measured.top) +
                margin * 2,
            84,
            1024);

    try {
        Bitmap bitmap(width, height);
        std::fill(
            bitmap.pixels.begin(),
            bitmap.pixels.end(),
            static_cast<std::uint8_t>(255));

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        OwnedGdiObject native(CreateDIBSection(
            screen.get(),
            &info,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0));
        if (!native.get() || !bits) {
            return std::nullopt;
        }
        std::memset(bits, 0xff, bitmap.pixels.size());
        MemoryDc memory(screen.get());
        SelectedObject selected_bitmap(memory.get(), native.get());
        SelectedObject selected_font(memory.get(), font.get());
        if (!memory.get() || !selected_bitmap.valid() ||
            !selected_font.valid()) {
            return std::nullopt;
        }
        SetBkMode(memory.get(), TRANSPARENT);
        SetTextColor(memory.get(), RGB(29, 33, 41));
        RECT text_rect{margin, margin, width - margin, height - margin};
        DrawTextW(
            memory.get(),
            text.c_str(),
            static_cast<int>(text.size()),
            &text_rect,
            DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_EXPANDTABS);
        std::memcpy(bitmap.pixels.data(), bits, bitmap.pixels.size());
        bitmap.make_opaque();
        return bitmap;
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::wstring> clipboard_text() {
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        return std::nullopt;
    }
    const SIZE_T bytes = GlobalSize(data);
    if (bytes < sizeof(wchar_t)) {
        return std::nullopt;
    }
    GlobalLockView locked(static_cast<HGLOBAL>(data));
    if (!locked.get()) {
        return std::nullopt;
    }
    const auto* characters = static_cast<const wchar_t*>(locked.get());
    const std::size_t capacity = bytes / sizeof(wchar_t);
    const std::size_t scan_limit = std::min(
        capacity,
        kMaximumTextCharacters + 1);
    std::size_t length = 0;
    while (length < scan_limit &&
           characters[length] != L'\0') {
        ++length;
    }
    if (length == scan_limit &&
        capacity <= kMaximumTextCharacters) {
        return std::nullopt;
    }
    try {
        return std::wstring(
            characters,
            std::min(length, kMaximumTextCharacters));
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::wstring clipboard_error_message(DWORD error) {
    if (error == ERROR_SUCCESS) {
        return L"无法打开剪贴板，系统未提供错误信息。";
    }
    return std::format(
        L"无法打开剪贴板：{} (Win32 {})",
        windows_error_message(error),
        error);
}

enum class ClipboardSnapshotKind {
    encoded_image,
    dib,
};

struct ClipboardByteSnapshot {
    ClipboardSnapshotKind kind{};
    std::vector<std::uint8_t> bytes;
};

}  // namespace

std::optional<Bitmap> decode_local_image_file(
    const std::filesystem::path& path,
    std::wstring* error) {
    clear_error(error);
    if (path.empty()) {
        set_error(error, L"未指定要贴出的图片文件。");
        return std::nullopt;
    }
    if (!path.is_absolute()) {
        set_error(error, L"图片文件路径必须是绝对路径。");
        return std::nullopt;
    }
    if (PathIsNetworkPathW(path.c_str())) {
        set_error(
            error,
            L"为避免阻塞截图主程序，请先把网络图片复制到本地再贴图。");
        return std::nullopt;
    }

    std::error_code file_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(path, file_error);
    if (file_error || file_size == 0) {
        set_error(error, L"图片文件不存在、为空或无法读取。");
        return std::nullopt;
    }
    if (file_size > kClipboardPayloadBudget) {
        set_error(error, L"图片文件超过 128 MiB 安全上限。");
        return std::nullopt;
    }

    const ScopedWinrtApartment apartment(true);
    ComPtr<IWICImagingFactory> factory;
    if (!apartment.available() ||
        FAILED(create_wic_factory(factory)) || !factory) {
        set_error(error, L"Windows 图片解码器不可用。");
        return std::nullopt;
    }
    auto bitmap = decode_image_file(factory.Get(), path);
    if (!bitmap || !bitmap->valid()) {
        set_error(
            error,
            L"文件不是受支持的静态图片，或解码后超过 64 MiB 安全上限。");
        return std::nullopt;
    }
    return bitmap;
}

std::optional<ClipboardVisual> read_clipboard_visual(
    HWND owner,
    std::wstring* error) {
    clear_error(error);

    ClipboardOwner clipboard_owner(owner);
    if (!clipboard_owner.get()) {
        set_error(error, L"无法创建剪贴板访问窗口。");
        return std::nullopt;
    }

    std::vector<ClipboardByteSnapshot> byte_snapshots;
    std::size_t snapshot_budget_used = 0;
    OwnedBitmap snapshot_bitmap;
    std::filesystem::path snapshot_path;
    std::wstring snapshot_text;
    {
        ClipboardScope clipboard(clipboard_owner.get());
        if (!clipboard.open()) {
            set_error(
                error,
                clipboard_error_message(clipboard.error()));
            return std::nullopt;
        }

        const auto snapshot_global =
            [&](ClipboardSnapshotKind kind,
                HGLOBAL memory,
                std::size_t candidate_budget) {
                const std::size_t remaining =
                    kClipboardPayloadBudget -
                    std::min(
                        snapshot_budget_used,
                        kClipboardPayloadBudget);
                if (remaining == 0) {
                    return false;
                }
                auto bytes = copy_global_bytes(
                    memory,
                    std::min(remaining, candidate_budget));
                if (!bytes) {
                    return false;
                }
                snapshot_budget_used += bytes->size();
                byte_snapshots.push_back(
                    {kind, std::move(*bytes)});
                return true;
            };

        constexpr std::array<const wchar_t*, 2> encoded_formats{
            L"PNG",
            L"image/png",
        };
        for (const wchar_t* name : encoded_formats) {
            const UINT format = RegisterClipboardFormatW(name);
            if (format == 0 ||
                !IsClipboardFormatAvailable(format)) {
                continue;
            }
            if (snapshot_global(
                    ClipboardSnapshotKind::encoded_image,
                    static_cast<HGLOBAL>(
                        GetClipboardData(format)),
                    kDecodedVisualBudget)) {
                break;
            }
        }

        constexpr std::array<UINT, 2> dib_formats{
            CF_DIBV5,
            CF_DIB,
        };
        for (const UINT format : dib_formats) {
            if (!IsClipboardFormatAvailable(format)) {
                continue;
            }
            (void)snapshot_global(
                ClipboardSnapshotKind::dib,
                static_cast<HGLOBAL>(
                    GetClipboardData(format)),
                kDecodedVisualBudget +
                    sizeof(BITMAPV5HEADER));
        }

        const bool has_dib_snapshot =
            std::ranges::any_of(
                byte_snapshots,
                [](const ClipboardByteSnapshot& snapshot) {
                    return snapshot.kind ==
                           ClipboardSnapshotKind::dib;
                });
        if (!has_dib_snapshot &&
            IsClipboardFormatAvailable(CF_BITMAP)) {
            const auto source = static_cast<HBITMAP>(
                GetClipboardData(CF_BITMAP));
            BITMAP native{};
            if (source &&
                GetObjectW(
                    source,
                    static_cast<int>(sizeof(native)),
                    &native) ==
                    static_cast<int>(sizeof(native)) &&
                native.bmWidth > 0 &&
                native.bmHeight != 0 &&
                native.bmHeight !=
                    std::numeric_limits<LONG>::min()) {
                const int width =
                    static_cast<int>(native.bmWidth);
                const int height =
                    static_cast<int>(std::abs(native.bmHeight));
                const auto decoded_size =
                    Bitmap::checked_byte_size(width, height);
                if (decoded_size &&
                    *decoded_size <= kDecodedVisualBudget &&
                    *decoded_size <=
                        kClipboardPayloadBudget -
                            std::min(
                                snapshot_budget_used,
                                kClipboardPayloadBudget)) {
                    snapshot_bitmap = OwnedBitmap(
                        static_cast<HBITMAP>(CopyImage(
                            source,
                            IMAGE_BITMAP,
                            0,
                            0,
                            LR_CREATEDIBSECTION)));
                    if (snapshot_bitmap) {
                        snapshot_budget_used +=
                            *decoded_size;
                    }
                }
            }
        }

        if (IsClipboardFormatAvailable(CF_HDROP)) {
            const auto drop =
                static_cast<HDROP>(GetClipboardData(CF_HDROP));
            if (drop &&
                DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) > 0) {
                const UINT length =
                    DragQueryFileW(drop, 0, nullptr, 0);
                if (length > 0 && length < 32768) {
                    std::wstring path(length + 1, L'\0');
                    if (DragQueryFileW(
                            drop,
                            0,
                            path.data(),
                            static_cast<UINT>(path.size())) == length) {
                        path.resize(length);
                        snapshot_path = std::move(path);
                    }
                }
            }
        }

        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (auto text = clipboard_text()) {
                snapshot_text = std::move(*text);
            }
        }
    }

    if (byte_snapshots.empty() &&
        !snapshot_bitmap &&
        snapshot_path.empty() &&
        snapshot_text.empty()) {
        set_error(
            error,
            L"剪贴板中没有可贴出的图像、图片文件、颜色或文本。");
        return std::nullopt;
    }

    const ScopedWinrtApartment apartment(true);
    ComPtr<IWICImagingFactory> factory;
    HRESULT factory_result = E_FAIL;
    if (apartment.available()) {
        factory_result = create_wic_factory(factory);
    }
    const bool factory_ready =
        SUCCEEDED(factory_result) && factory;

    for (const auto& snapshot : byte_snapshots) {
        std::optional<Bitmap> decoded;
        if (snapshot.kind ==
            ClipboardSnapshotKind::encoded_image) {
            if (factory_ready) {
                decoded = decode_image_bytes(
                    factory.Get(),
                    snapshot.bytes);
            }
        } else {
            decoded = decode_dib_bytes(snapshot.bytes);
        }
        if (decoded && decoded->valid()) {
            return ClipboardVisual{
                std::move(*decoded),
                ClipboardVisualKind::image,
                L"剪贴板图像",
            };
        }
    }

    if (snapshot_bitmap && factory_ready) {
        if (auto decoded = decode_hbitmap(
                factory.Get(),
                snapshot_bitmap.get());
            decoded && decoded->valid()) {
            return ClipboardVisual{
                std::move(*decoded),
                ClipboardVisualKind::image,
                L"剪贴板图像",
            };
        }
    }

    std::wstring file_failure;
    if (!snapshot_path.empty()) {
        if (PathIsNetworkPathW(snapshot_path.c_str())) {
            file_failure =
                L"为避免阻塞截图主程序，请先把网络图片复制到本地再贴图。";
        } else {
            std::error_code file_error;
            const std::uintmax_t file_size =
                std::filesystem::file_size(
                    snapshot_path,
                    file_error);
            if (file_error || file_size == 0 ||
                file_size > kClipboardPayloadBudget) {
                file_failure =
                    L"剪贴板中的图片文件不可读取，或超过 128 MiB 安全上限。";
            } else if (factory_ready) {
                if (auto decoded = decode_image_file(
                        factory.Get(),
                        snapshot_path);
                    decoded && decoded->valid()) {
                    return ClipboardVisual{
                        std::move(*decoded),
                        ClipboardVisualKind::image_file,
                        snapshot_path.filename().wstring(),
                    };
                }
                file_failure =
                    L"剪贴板中的文件不是受支持的图片，或图片过大。";
            }
        }
    }

    if (!snapshot_text.empty()) {
        const std::wstring normalized = trim(snapshot_text);
        if (!normalized.empty()) {
            if (const auto color = parse_color(normalized)) {
                const std::wstring label =
                    canonical_color(*color);
                if (auto bitmap =
                        render_color_card(*color, label)) {
                    return ClipboardVisual{
                        std::move(*bitmap),
                        ClipboardVisualKind::color,
                        label,
                    };
                }
            }
            if (auto bitmap =
                    render_text_card(snapshot_text)) {
                return ClipboardVisual{
                    std::move(*bitmap),
                    ClipboardVisualKind::text,
                    L"剪贴板文本",
                };
            }
        }
    }

    if (!file_failure.empty()) {
        set_error(error, std::move(file_failure));
        return std::nullopt;
    }
    if ((!byte_snapshots.empty() || snapshot_bitmap) &&
        !factory_ready &&
        std::ranges::none_of(
            byte_snapshots,
            [](const ClipboardByteSnapshot& snapshot) {
                return snapshot.kind ==
                       ClipboardSnapshotKind::dib;
            })) {
        set_error(
            error,
            L"无法初始化 Windows 图像解码器：" +
                windows_error_message(
                    static_cast<DWORD>(factory_result)));
        return std::nullopt;
    }
    set_error(
        error,
        !byte_snapshots.empty() || snapshot_bitmap
            ? L"剪贴板图像已损坏、格式不受支持或尺寸过大。"
            : L"剪贴板文本为空、过大或无法渲染。");
    return std::nullopt;
}

}  // namespace airshot
