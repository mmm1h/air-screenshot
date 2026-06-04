#include "airshot/ocr.h"

#include <robuffer.h>

#include <cstring>

#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.h>

namespace airshot {
namespace {

struct __declspec(uuid("905a0fef-bc53-11df-8c49-001e4fc686da")) BufferByteAccess : IUnknown {
    virtual HRESULT __stdcall Buffer(std::uint8_t** value) = 0;
};

}  // namespace

std::wstring join_ocr_lines(std::span<const std::wstring> lines) {
    std::wstring result;
    for (const auto& line : lines) {
        if (!result.empty()) {
            result += L"\r\n";
        }
        result += line;
    }
    return result;
}

OcrOutput recognize_text(const Bitmap& bitmap) {
    if (bitmap.empty()) {
        return {false, {}, L"OCR 图像为空。"};
    }
    const ScopedWinrtApartment apartment(true);
    if (!apartment.available()) {
        return {false, {}, L"无法初始化 Windows Runtime。"};
    }
    try {
        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Media::Ocr;
        using namespace winrt::Windows::Storage::Streams;

        if (bitmap.width > static_cast<int>(OcrEngine::MaxImageDimension()) ||
            bitmap.height > static_cast<int>(OcrEngine::MaxImageDimension())) {
            return {false, {}, L"选区尺寸超过 Windows OCR 支持范围。"};
        }

        Buffer buffer(static_cast<std::uint32_t>(bitmap.pixels.size()));
        buffer.Length(static_cast<std::uint32_t>(bitmap.pixels.size()));
        std::uint8_t* target = nullptr;
        winrt::check_hresult(buffer.as<BufferByteAccess>()->Buffer(&target));
        std::memcpy(target, bitmap.pixels.data(), bitmap.pixels.size());

        const SoftwareBitmap software_bitmap = SoftwareBitmap::CreateCopyFromBuffer(
            buffer, BitmapPixelFormat::Bgra8, bitmap.width, bitmap.height, BitmapAlphaMode::Premultiplied);
        const OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            return {false, {}, L"系统没有可用的 OCR 语言包。"};
        }
        const auto result = engine.RecognizeAsync(software_bitmap).get();
        std::wstring text = result.Text().c_str();
        if (text.empty()) {
            return {false, {}, L"未识别到文本。"};
        }
        return {true, std::move(text), {}};
    } catch (const winrt::hresult_error& error) {
        return {false, {}, std::format(L"Windows OCR 不可用：{}", error.message().c_str())};
    } catch (const std::exception& error) {
        return {false, {}, std::format(L"OCR 失败：{}", from_utf8(error.what()))};
    }
}

}  // namespace airshot
