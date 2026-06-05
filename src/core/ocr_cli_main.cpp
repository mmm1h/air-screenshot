#include "airshot/bitmap.h"
#include "airshot/config.h"
#include <windows.h>
#include <wincodec.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <robuffer.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <cwchar>
#include <io.h>
#include <fcntl.h>

namespace {

// ComPtr helper template
template <typename T>
class ComPtr {
public:
    ComPtr() : ptr_(nullptr) {}
    ComPtr(T* ptr) : ptr_(ptr) { if (ptr_) ptr_->AddRef(); }
    ~ComPtr() { Reset(); }
    T* Get() const { return ptr_; }
    T** GetAddressOf() { Reset(); return &ptr_; }
    void Reset() { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } }
    T* operator->() const { return ptr_; }
    operator bool() const { return ptr_ != nullptr; }
private:
    T* ptr_;
};

struct __declspec(uuid("905a0fef-bc53-11df-8c49-001e4fc686da")) BufferByteAccess : IUnknown {
    virtual HRESULT __stdcall Buffer(std::uint8_t** value) = 0;
};

// Image Loader using WIC
bool load_image(const std::filesystem::path& path, airshot::Bitmap& bitmap) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    }
    if (FAILED(result)) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(result)) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(result)) return false;

    UINT width = 0, height = 0;
    result = frame->GetSize(&width, &height);
    if (FAILED(result)) return false;

    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(result)) return false;

    result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
    if (FAILED(result)) return false;

    bitmap = airshot::Bitmap(static_cast<int>(width), static_cast<int>(height));
    result = converter->CopyPixels(nullptr, static_cast<UINT>(bitmap.stride()), static_cast<UINT>(bitmap.pixels.size()), bitmap.pixels.data());
    return SUCCEEDED(result);
}

// System WinRT OCR Implementation
bool run_winrt_ocr(const airshot::Bitmap& bitmap, std::wstring& out_text, std::wstring& out_error) {
    try {
        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Media::Ocr;
        using namespace winrt::Windows::Storage::Streams;

        if (bitmap.width > static_cast<int>(OcrEngine::MaxImageDimension()) ||
            bitmap.height > static_cast<int>(OcrEngine::MaxImageDimension())) {
            out_error = L"图像尺寸超出了 Windows OCR 支持的最大范围。";
            return false;
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
            out_error = L"系统未安装任何 OCR 语言包。";
            return false;
        }
        const auto result = engine.RecognizeAsync(software_bitmap).get();
        out_text = result.Text().c_str();
        return true;
    } catch (const winrt::hresult_error& error) {
        out_error = L"Windows OCR 运行错误: " + std::wstring(error.message().c_str());
        return false;
    } catch (const std::exception& error) {
        out_error = L"OCR 异常: " + std::wstring(error.what(), error.what() + std::strlen(error.what()));
        return false;
    }
}

// WeChat OCR DLL Signatures
typedef bool (__stdcall* FnWeChatOCRInit)(const wchar_t* wechat_dir, const wchar_t* ocr_dir);
typedef wchar_t* (__stdcall* FnWeChatOCRRecognize)(const wchar_t* image_path);
typedef void (__stdcall* FnWeChatOCRFreeResult)(wchar_t* text);

bool run_wechat_ocr(const std::filesystem::path& dll_path, const std::filesystem::path& image_path,
                    const std::wstring& wechat_dir, const std::wstring& ocr_dir,
                    std::wstring& out_text, std::wstring& out_error) {
    HMODULE hModule = LoadLibraryW(dll_path.c_str());
    if (!hModule) {
        out_error = L"无法加载 wechat_ocr_api.dll (错误码 " + std::to_wstring(GetLastError()) + L")。";
        return false;
    }

    auto fn_init = reinterpret_cast<FnWeChatOCRInit>(GetProcAddress(hModule, "WeChatOCR_Init"));
    auto fn_recognize = reinterpret_cast<FnWeChatOCRRecognize>(GetProcAddress(hModule, "WeChatOCR_Recognize"));
    auto fn_free = reinterpret_cast<FnWeChatOCRFreeResult>(GetProcAddress(hModule, "WeChatOCR_FreeResult"));

    if (!fn_init || !fn_recognize || !fn_free) {
        out_error = L"wechat_ocr_api.dll 中缺少必要的导出函数。";
        FreeLibrary(hModule);
        return false;
    }

    if (!fn_init(wechat_dir.c_str(), ocr_dir.c_str())) {
        out_error = L"微信 OCR 引擎初始化失败。";
        FreeLibrary(hModule);
        return false;
    }

    wchar_t* result_str = fn_recognize(image_path.c_str());
    if (result_str) {
        out_text = result_str;
        fn_free(result_str);
    } else {
        out_error = L"微信 OCR 未识别到任何文本。";
        FreeLibrary(hModule);
        return false;
    }

    FreeLibrary(hModule);
    return true;
}

// ONNX OCR DLL Signatures
typedef bool (__stdcall* FnONNXOCRInit)(const wchar_t* model_dir);
typedef wchar_t* (__stdcall* FnONNXOCRRecognize)(const wchar_t* image_path);
typedef void (__stdcall* FnONNXOCRFreeResult)(wchar_t* text);
typedef void (__stdcall* FnONNXOCRSetThreadCount)(int thread_count);

bool run_onnx_ocr(const std::filesystem::path& dll_path, const std::filesystem::path& image_path,
                  const std::wstring& model_dir, int ort_threads, std::wstring& out_text, std::wstring& out_error) {
    SetDllDirectoryW(dll_path.parent_path().c_str());
    const std::wstring thread_count = std::to_wstring(std::max(1, ort_threads));
    SetEnvironmentVariableW(L"OMP_NUM_THREADS", thread_count.c_str());
    SetEnvironmentVariableW(L"OMP_WAIT_POLICY", L"PASSIVE");

    HMODULE hModule = LoadLibraryW(dll_path.c_str());
    if (!hModule) {
        out_error = L"无法加载 rapidocr_api.dll (错误码 " + std::to_wstring(GetLastError()) + L")。";
        SetDllDirectoryW(nullptr);
        return false;
    }

    auto fn_init = reinterpret_cast<FnONNXOCRInit>(GetProcAddress(hModule, "ONNXOCR_Init"));
    auto fn_recognize = reinterpret_cast<FnONNXOCRRecognize>(GetProcAddress(hModule, "ONNXOCR_Recognize"));
    auto fn_free = reinterpret_cast<FnONNXOCRFreeResult>(GetProcAddress(hModule, "ONNXOCR_FreeResult"));
    auto fn_set_threads = reinterpret_cast<FnONNXOCRSetThreadCount>(GetProcAddress(hModule, "ONNXOCR_SetThreadCount"));

    if (!fn_init || !fn_recognize || !fn_free) {
        out_error = L"rapidocr_api.dll 中缺少必要的导出函数。";
        FreeLibrary(hModule);
        SetDllDirectoryW(nullptr);
        return false;
    }

    if (fn_set_threads) {
        fn_set_threads(std::max(1, ort_threads));
    }

    if (!fn_init(model_dir.c_str())) {
        out_error = L"ONNX OCR 引擎模型载入失败。";
        FreeLibrary(hModule);
        SetDllDirectoryW(nullptr);
        return false;
    }

    wchar_t* result_str = fn_recognize(image_path.c_str());
    if (result_str) {
        out_text = result_str;
        fn_free(result_str);
    } else {
        out_error = L"本地 ONNX OCR 未识别到任何文本。";
        FreeLibrary(hModule);
        SetDllDirectoryW(nullptr);
        return false;
    }

    FreeLibrary(hModule);
    SetDllDirectoryW(nullptr);
    return true;
}

std::filesystem::path get_dependency_file_path(const wchar_t* argv0, const std::wstring& dependency_dir, const wchar_t* relative_path) {
    if (!dependency_dir.empty()) {
        std::filesystem::path explicit_path = std::filesystem::path(dependency_dir) / relative_path;
        if (std::filesystem::exists(explicit_path)) {
            return explicit_path;
        }
    }

    // 1. Check current exe folder
    std::filesystem::path current_dir = std::filesystem::path(argv0).parent_path();
    const std::filesystem::path packaged_path = current_dir / L"ocr" / airshot::kRapidOcrPackageId / relative_path;
    if (std::filesystem::exists(packaged_path)) {
        return packaged_path;
    }
    const std::filesystem::path legacy_local_path = current_dir / L"ocr_onnx" / relative_path;
    if (std::filesystem::exists(legacy_local_path)) {
        return legacy_local_path;
    }

    // 2. Check AppData folder
    const wchar_t* local_appdata = _wgetenv(L"LOCALAPPDATA");
    if (local_appdata) {
        const std::filesystem::path app_data_path =
            std::filesystem::path(local_appdata) / L"AirScreenshot" / L"ocr" / airshot::kRapidOcrPackageId / relative_path;
        if (std::filesystem::exists(app_data_path)) {
            return app_data_path;
        }
        const std::filesystem::path legacy_app_data_path =
            std::filesystem::path(local_appdata) / L"AirScreenshot" / L"ocr_onnx" / relative_path;
        if (std::filesystem::exists(legacy_app_data_path)) {
            return legacy_app_data_path;
        }
    }
    return legacy_local_path; // fallback
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    // Enable UTF-16 stdout output mode to prevent console corruption
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    std::wstring engine = L"winrt";
    std::wstring image_path;
    std::wstring wechat_dir;
    std::wstring ocr_dir;
    std::wstring model_dir;
    std::wstring dependency_dir;
    int ort_threads = 2;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--engine" && i + 1 < argc) {
            engine = argv[++i];
        } else if (arg == L"--image" && i + 1 < argc) {
            image_path = argv[++i];
        } else if (arg == L"--wechat-dir" && i + 1 < argc) {
            wechat_dir = argv[++i];
        } else if (arg == L"--ocr-dir" && i + 1 < argc) {
            ocr_dir = argv[++i];
        } else if (arg == L"--model-dir" && i + 1 < argc) {
            model_dir = argv[++i];
        } else if (arg == L"--dependency-dir" && i + 1 < argc) {
            dependency_dir = argv[++i];
        } else if (arg == L"--ort-threads" && i + 1 < argc) {
            try {
                ort_threads = std::max(1, std::stoi(argv[++i]));
            } catch (...) {
                ort_threads = 2;
            }
        }
    }

    if (image_path.empty()) {
        std::wcerr << L"错误: 缺少 --image 参数。\n";
        return 1;
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    std::wstring out_text;
    std::wstring out_error;
    bool success = false;

    if (engine == L"winrt") {
        airshot::Bitmap bitmap;
        if (!load_image(image_path, bitmap)) {
            std::wcerr << L"错误: 无法加载图像: " << image_path << L"\n";
            return 1;
        }
        success = run_winrt_ocr(bitmap, out_text, out_error);
    } else if (engine == L"wechat") {
        std::filesystem::path dll_path = get_dependency_file_path(argv[0], dependency_dir, L"wechat_ocr_api.dll");
        success = run_wechat_ocr(dll_path, image_path, wechat_dir, ocr_dir, out_text, out_error);
    } else if (engine == L"onnx") {
        std::filesystem::path dll_path = get_dependency_file_path(argv[0], dependency_dir, L"rapidocr_api.dll");
        if (model_dir.empty()) {
            model_dir = (dll_path.parent_path() / L"models").wstring();
        }
        success = run_onnx_ocr(dll_path, image_path, model_dir, ort_threads, out_text, out_error);
    } else {
        std::wcerr << L"错误: 未知的 OCR 引擎 \"" << engine << L"\"。\n";
        return 1;
    }

    if (success) {
        std::wcout << out_text;
        return 0;
    } else {
        std::wcerr << out_error;
        return 2;
    }
}
