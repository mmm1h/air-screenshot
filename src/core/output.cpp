#include "airshot/output.h"

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
    if (bitmap.empty()) {
        if (error) {
            *error = L"图像为空。";
        }
        return false;
    }

    // Encode PNG first before opening clipboard to minimize open time
    HGLOBAL png_memory = encode_png_to_hglobal(bitmap);
    UINT png_format = RegisterClipboardFormatW(L"PNG");

    if (!open_clipboard_with_retry(nullptr)) {
        if (png_memory) {
            GlobalFree(png_memory);
        }
        if (error) {
            *error = L"剪贴板正被其他程序占用。";
        }
        return false;
    }

    EmptyClipboard();

    // 1. Set registered "PNG" clipboard format (for Electron/Chromium applications)
    if (png_memory) {
        if (!SetClipboardData(png_format, png_memory)) {
            GlobalFree(png_memory);
        }
    }

    // 2. Set CF_DIBV5 (Device-Independent Bitmap V5 - supports alpha channel)
    const SIZE_T total_size = sizeof(BITMAPV5HEADER) + bitmap.pixels.size();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, total_size);
    if (memory) {
        auto* data = static_cast<std::uint8_t*>(GlobalLock(memory));
        BITMAPV5HEADER header{};
        header.bV5Size = sizeof(header);
        header.bV5Width = bitmap.width;
        header.bV5Height = -bitmap.height;
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
        std::memcpy(data + sizeof(header), bitmap.pixels.data(), bitmap.pixels.size());
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_DIBV5, memory)) {
            GlobalFree(memory);
        }
    }

    // 3. Set CF_DIB (standard Device-Independent Bitmap - widely supported by Slack, WeChat, browser, etc.)
    const SIZE_T dib_size = sizeof(BITMAPINFOHEADER) + bitmap.pixels.size();
    HGLOBAL dib_memory = GlobalAlloc(GMEM_MOVEABLE, dib_size);
    if (dib_memory) {
        auto* dib_data = static_cast<std::uint8_t*>(GlobalLock(dib_memory));
        BITMAPINFOHEADER dib_header{};
        dib_header.biSize = sizeof(dib_header);
        dib_header.biWidth = bitmap.width;
        dib_header.biHeight = -bitmap.height;
        dib_header.biPlanes = 1;
        dib_header.biBitCount = 32;
        dib_header.biCompression = BI_RGB;
        dib_header.biSizeImage = static_cast<DWORD>(bitmap.pixels.size());
        std::memcpy(dib_data, &dib_header, sizeof(dib_header));
        std::memcpy(dib_data + sizeof(dib_header), bitmap.pixels.data(), bitmap.pixels.size());
        GlobalUnlock(dib_memory);
        if (!SetClipboardData(CF_DIB, dib_memory)) {
            GlobalFree(dib_memory);
        }
    }

    // 4. Set CF_BITMAP (Device-Dependent Bitmap - classic format)
    HDC screen = GetDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap.width;
    info.bmiHeader.biHeight = -bitmap.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbitmap && bits) {
        std::memcpy(bits, bitmap.pixels.data(), bitmap.pixels.size());
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
