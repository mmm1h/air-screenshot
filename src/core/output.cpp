#include "airshot/output.h"

#include "airshot/common.h"
#include "airshot/config.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <ole2.h>

#include <cstring>

namespace airshot {
namespace {

using Microsoft::WRL::ComPtr;

bool open_clipboard_with_retry(HWND owner) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (OpenClipboard(owner)) {
            return true;
        }
        Sleep(15);
    }
    return false;
}

std::filesystem::path pictures_directory() {
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &value)) && value) {
        std::filesystem::path result(value);
        CoTaskMemFree(value);
        return result;
    }
    return config_directory();
}

HGLOBAL encode_png_to_hglobal(const Bitmap& bitmap) {
    if (bitmap.empty()) return nullptr;

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    }
    if (FAILED(result)) return nullptr;

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return nullptr;

    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;

    result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
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
    if (SUCCEEDED(result) && format == GUID_WICPixelFormat32bppBGRA) {
        result = frame->WritePixels(static_cast<UINT>(bitmap.height),
                                    static_cast<UINT>(bitmap.stride()),
                                    static_cast<UINT>(bitmap.pixels.size()),
                                    const_cast<BYTE*>(bitmap.pixels.data()));
    }
    if (SUCCEEDED(result)) {
        result = frame->Commit();
    }
    if (SUCCEEDED(result)) {
        result = encoder->Commit();
    }

    HGLOBAL hg = nullptr;
    if (SUCCEEDED(result)) {
        HGLOBAL stream_hg = nullptr;
        if (SUCCEEDED(GetHGlobalFromStream(stream, &stream_hg))) {
            SIZE_T size = GlobalSize(stream_hg);
            hg = GlobalAlloc(GMEM_MOVEABLE, size);
            if (hg) {
                void* src = GlobalLock(stream_hg);
                void* dst = GlobalLock(hg);
                if (src && dst) {
                    std::memcpy(dst, src, size);
                }
                if (src) GlobalUnlock(stream_hg);
                if (dst) GlobalUnlock(hg);
            }
        }
    }

    stream->Release();
    return hg;
}

}  // namespace

bool copy_bitmap_to_clipboard(const Bitmap& bitmap, std::wstring* error) {
    const ScopedWinrtApartment apartment(true);
    if (bitmap.empty()) {
        if (error) {
            *error = L"图像为空。";
        }
        return false;
    }

    const int W = bitmap.width;
    const int H = bitmap.height;

    // Force alpha channel to 255 (fully opaque) to prevent Electron-based apps (like Feishu, Slack)
    // or standard apps (like WeChat) from pasting the image as fully transparent/black/blank.
    Bitmap opaque_bitmap = bitmap;
    for (std::size_t i = 3; i < opaque_bitmap.pixels.size(); i += 4) {
        opaque_bitmap.pixels[i] = 255;
    }

    // Encode PNG first before opening clipboard to minimize open time
    HGLOBAL png_memory = encode_png_to_hglobal(opaque_bitmap);
    HGLOBAL png_memory_alt = nullptr;
    if (png_memory) {
        SIZE_T png_size = GlobalSize(png_memory);
        png_memory_alt = GlobalAlloc(GMEM_MOVEABLE, png_size);
        if (png_memory_alt) {
            void* src = GlobalLock(png_memory);
            void* dst = GlobalLock(png_memory_alt);
            if (src && dst) {
                std::memcpy(dst, src, png_size);
            }
            if (src) GlobalUnlock(png_memory);
            if (dst) GlobalUnlock(png_memory_alt);
        }
    }
    UINT png_format = RegisterClipboardFormatW(L"PNG");
    UINT png_format_alt = RegisterClipboardFormatW(L"image/png");

    if (!open_clipboard_with_retry(nullptr)) {
        if (png_memory) {
            GlobalFree(png_memory);
        }
        if (png_memory_alt) {
            GlobalFree(png_memory_alt);
        }
        if (error) {
            *error = L"剪贴板正被其他程序占用。";
        }
        return false;
    }

    EmptyClipboard();

    // 1. Set registered "PNG" and "image/png" clipboard formats (for Electron/Chromium applications)
    if (png_memory) {
        if (!SetClipboardData(png_format, png_memory)) {
            GlobalFree(png_memory);
        }
    }
    if (png_memory_alt) {
        if (!SetClipboardData(png_format_alt, png_memory_alt)) {
            GlobalFree(png_memory_alt);
        }
    }

    // 2. Set CF_DIBV5 (Device-Independent Bitmap V5 - supports alpha channel, bottom-up format)
    const SIZE_T total_size = sizeof(BITMAPV5HEADER) + opaque_bitmap.pixels.size();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, total_size);
    if (memory) {
        auto* data = static_cast<std::uint8_t*>(GlobalLock(memory));
        BITMAPV5HEADER header{};
        header.bV5Size = sizeof(header);
        header.bV5Width = W;
        header.bV5Height = H; // Positive height = bottom-up DIB
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5SizeImage = static_cast<DWORD>(opaque_bitmap.pixels.size());
        header.bV5RedMask = 0x00FF0000;
        header.bV5GreenMask = 0x0000FF00;
        header.bV5BlueMask = 0x000000FF;
        header.bV5AlphaMask = 0xFF000000;
        header.bV5CSType = LCS_sRGB;
        std::memcpy(data, &header, sizeof(header));

        auto* dest = data + sizeof(header);
        for (int row = H - 1; row >= 0; --row) { // Bottom-up copy
            const auto* src_row = opaque_bitmap.row(row).data();
            auto* dest_row = dest + static_cast<std::size_t>(H - 1 - row) * W * 4U;
            std::memcpy(dest_row, src_row, static_cast<std::size_t>(W) * 4U);
        }
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_DIBV5, memory)) {
            GlobalFree(memory);
        }
    }

    // 3. Set CF_DIB (standard Device-Independent Bitmap, converted to 24-bit BGR bottom-up format for maximum compatibility)
    const int stride24 = ((W * 24 + 31) / 32) * 4;
    const SIZE_T dib_size = sizeof(BITMAPINFOHEADER) + static_cast<SIZE_T>(stride24) * H;
    HGLOBAL dib_memory = GlobalAlloc(GMEM_MOVEABLE, dib_size);
    if (dib_memory) {
        auto* dib_data = static_cast<std::uint8_t*>(GlobalLock(dib_memory));
        BITMAPINFOHEADER dib_header{};
        dib_header.biSize = sizeof(dib_header);
        dib_header.biWidth = W;
        dib_header.biHeight = H; // Positive height = bottom-up DIB
        dib_header.biPlanes = 1;
        dib_header.biBitCount = 24; // 24-bit BGR is universally supported
        dib_header.biCompression = BI_RGB;
        dib_header.biSizeImage = static_cast<DWORD>(stride24 * H);
        std::memcpy(dib_data, &dib_header, sizeof(dib_header));

        auto* dest = dib_data + sizeof(dib_header);
        for (int row = H - 1; row >= 0; --row) { // Bottom-up 32-to-24 bit copy
            const auto* src_row = opaque_bitmap.row(row).data();
            auto* dest_row = dest + static_cast<std::size_t>(H - 1 - row) * stride24;
            for (int col = 0; col < W; ++col) {
                const int src_idx = col * 4;
                const int dest_idx = col * 3;
                dest_row[dest_idx] = src_row[src_idx];       // Blue
                dest_row[dest_idx + 1] = src_row[src_idx + 1]; // Green
                dest_row[dest_idx + 2] = src_row[src_idx + 2]; // Red
            }
        }
        GlobalUnlock(dib_memory);
        if (!SetClipboardData(CF_DIB, dib_memory)) {
            GlobalFree(dib_memory);
        }
    }

    // 4. Set CF_BITMAP (Device-Dependent Bitmap - classic bottom-up format)
    HDC screen = GetDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = W;
    info.bmiHeader.biHeight = H; // Positive height = bottom-up DIB
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbitmap && bits) {
        auto* dest = static_cast<std::uint8_t*>(bits);
        for (int row = H - 1; row >= 0; --row) { // Bottom-up copy
            const auto* src_row = opaque_bitmap.row(row).data();
            auto* dest_row = dest + static_cast<std::size_t>(H - 1 - row) * W * 4U;
            std::memcpy(dest_row, src_row, static_cast<std::size_t>(W) * 4U);
        }
        if (!SetClipboardData(CF_BITMAP, hbitmap)) {
            DeleteObject(hbitmap);
        }
    }
    ReleaseDC(nullptr, screen);

    CloseClipboard();
    return true;
}

bool copy_text_to_clipboard(std::wstring_view text, std::wstring* error) {
    const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        if (error) {
            *error = windows_error_message(GetLastError());
        }
        return false;
    }
    void* data = GlobalLock(memory);
    std::memcpy(data, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(data)[text.size()] = L'\0';
    GlobalUnlock(memory);

    if (!open_clipboard_with_retry(nullptr)) {
        GlobalFree(memory);
        if (error) {
            *error = L"剪贴板正被其他程序占用。";
        }
        return false;
    }
    EmptyClipboard();
    const HANDLE result = SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!result) {
        GlobalFree(memory);
        if (error) {
            *error = windows_error_message(GetLastError());
        }
        return false;
    }
    return true;
}

bool save_png(const Bitmap& bitmap, const std::filesystem::path& path, std::wstring* error) {
    const ScopedWinrtApartment apartment(true);
    if (bitmap.empty()) {
        if (error) {
            *error = L"图像为空。";
        }
        return false;
    }
    try {
        std::filesystem::create_directories(path.parent_path());
    } catch (...) {
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        result =
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    }
    if (FAILED(result)) {
        if (error) {
            *error = windows_error_message(static_cast<DWORD>(result));
        }
        return false;
    }

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    result = factory->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(result)) {
        result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    }
    if (SUCCEEDED(result)) {
        result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    }
    if (SUCCEEDED(result)) {
        result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
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
    if (SUCCEEDED(result) && format == GUID_WICPixelFormat32bppBGRA) {
        result = frame->WritePixels(static_cast<UINT>(bitmap.height),
                                    static_cast<UINT>(bitmap.stride()),
                                    static_cast<UINT>(bitmap.pixels.size()),
                                    const_cast<BYTE*>(bitmap.pixels.data()));
    }
    if (SUCCEEDED(result)) {
        result = frame->Commit();
    }
    if (SUCCEEDED(result)) {
        result = encoder->Commit();
    }
    if (FAILED(result) && error) {
        *error = windows_error_message(static_cast<DWORD>(result));
    }
    return SUCCEEDED(result);
}

std::filesystem::path resolve_output_path(std::wstring_view requested) {
    if (requested.empty()) {
        const auto directory = pictures_directory() / L"Air Screenshot";
        return directory / std::format(L"AirShot-{}.png", timestamp_for_file());
    }
    std::filesystem::path path(requested);
    std::error_code code;
    if (std::filesystem::is_directory(path, code) || !path.has_extension()) {
        return path / std::format(L"AirShot-{}.png", timestamp_for_file());
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
