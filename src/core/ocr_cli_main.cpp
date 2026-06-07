#include "airshot/config.h"
#include "airshot/portable.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <span>
#include <fcntl.h>
#include <io.h>

namespace {

constexpr DWORD kRunnerTimeoutMs = 85'000;

typedef bool (__stdcall* FnONNXOCRInit)(const wchar_t* model_dir);
typedef bool (__stdcall* FnONNXOCRInitProfile)(const wchar_t* model_dir, const wchar_t* profile);
typedef wchar_t* (__stdcall* FnONNXOCRRecognize)(const wchar_t* image_path);
typedef void (__stdcall* FnONNXOCRFreeResult)(wchar_t* text);
typedef void (__stdcall* FnONNXOCRSetThreadCount)(int thread_count);

typedef void* (__cdecl* FnOcrInit)(
    const char* det_model,
    const char* cls_model,
    const char* rec_model,
    const char* key_path,
    int thread_count);
typedef char (__cdecl* FnOcrDetect)(void* handle, const char* image_dir, const char* image_name, void* params);
typedef int (__cdecl* FnOcrGetLen)(void* handle);
typedef char (__cdecl* FnOcrGetResult)(void* handle, char* buffer, int length);
typedef void (__cdecl* FnOcrDestroy)(void* handle);

struct RapidOcrOnnxParam {
    int padding = 50;
    int maxSideLen = 1024;
    float boxScoreThresh = 0.5f;
    float boxThresh = 0.3f;
    float unClipRatio = 1.6f;
    int doAngle = 1;
    int mostAngle = 1;
};

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring from_utf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    return result;
}

std::wstring quote_argument(std::wstring_view value) {
    std::wstring result{L"\""};
    for (const wchar_t character : value) {
        if (character == L'"') {
            result += L"\\\"";
        } else {
            result.push_back(character);
        }
    }
    result.push_back(L'"');
    return result;
}

std::filesystem::path get_dependency_file_path(
    const std::wstring& dependency_dir,
    const wchar_t* relative_path) {
    if (!dependency_dir.empty()) {
        std::filesystem::path explicit_path = std::filesystem::path(dependency_dir) / relative_path;
        if (std::filesystem::exists(explicit_path)) {
            return explicit_path;
        }
    }

    const std::filesystem::path current_dir = airshot::portable_executable_path().parent_path();
    const std::filesystem::path packaged_path =
        current_dir / L"ocr" / airshot::kRapidOcrOnnxPackageId / relative_path;
    if (std::filesystem::exists(packaged_path)) {
        return packaged_path;
    }

    const wchar_t* local_appdata = _wgetenv(L"LOCALAPPDATA");
    if (local_appdata) {
        const std::filesystem::path app_data_path =
            std::filesystem::path(local_appdata) / L"AirScreenshot" / L"ocr" /
            airshot::kRapidOcrOnnxPackageId / relative_path;
        if (std::filesystem::exists(app_data_path)) {
            return app_data_path;
        }
    }

    return packaged_path;
}

bool run_external_runner(
    const std::filesystem::path& runner_path,
    const std::filesystem::path& image_path,
    const std::wstring& model_dir,
    const std::wstring& profile,
    int ort_threads,
    std::wstring& out_text,
    std::wstring& out_error) {
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;

    SECURITY_ATTRIBUTES sa_attr{};
    sa_attr.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa_attr.bInheritHandle = TRUE;
    if (!CreatePipe(&stdout_read, &stdout_write, &sa_attr, 0)) {
        out_error = L"创建 OCR runner 管道失败。";
        return true;
    }
    if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        out_error = L"设置 OCR runner 管道继承失败。";
        return true;
    }

    std::wstring cmd_line = quote_argument(runner_path.wstring());
    cmd_line += L" --image " + quote_argument(image_path.wstring());
    cmd_line += L" --model-dir " + quote_argument(model_dir);
    cmd_line += L" --ocr-profile " + quote_argument(profile);
    cmd_line += L" --ort-threads " + std::to_wstring(std::max(1, ort_threads));

    STARTUPINFOW startup{};
    startup.cb = sizeof(STARTUPINFOW);
    startup.hStdOutput = stdout_write;
    startup.hStdError = stdout_write;
    startup.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION process{};
    BOOL created = CreateProcessW(
        nullptr,
        cmd_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        runner_path.parent_path().c_str(),
        &startup,
        &process);

    CloseHandle(stdout_write);
    if (!created) {
        CloseHandle(stdout_read);
        out_error = L"无法启动 RapidOCR runner。";
        return true;
    }

    std::vector<char> buffer;
    char temp[4096];
    const ULONGLONG deadline = GetTickCount64() + kRunnerTimeoutMs;
    bool timed_out = false;

    while (true) {
        DWORD available = 0;
        if (PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD bytes_read = 0;
            const DWORD read_size = std::min<DWORD>(available, sizeof(temp));
            if (ReadFile(stdout_read, temp, read_size, &bytes_read, nullptr) && bytes_read > 0) {
                buffer.insert(buffer.end(), temp, temp + bytes_read);
            }
        }

        const DWORD wait_result = WaitForSingleObject(process.hProcess, 25);
        if (wait_result == WAIT_OBJECT_0) {
            while (PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                DWORD bytes_read = 0;
                const DWORD read_size = std::min<DWORD>(available, sizeof(temp));
                if (!ReadFile(stdout_read, temp, read_size, &bytes_read, nullptr) || bytes_read == 0) {
                    break;
                }
                buffer.insert(buffer.end(), temp, temp + bytes_read);
            }
            break;
        }
        if (GetTickCount64() >= deadline) {
            timed_out = true;
            TerminateProcess(process.hProcess, 124);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
    }

    CloseHandle(stdout_read);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    const std::string output(buffer.begin(), buffer.end());
    if (timed_out) {
        out_error = L"RapidOCR runner 超时，已停止本次识别。";
        return true;
    }
    if (exit_code != 0) {
        out_error = from_utf8(output);
        if (out_error.empty()) {
            out_error = L"RapidOCR runner 执行失败。";
        }
        return true;
    }

    out_text = from_utf8(output);
    if (out_text.empty()) {
        out_error = L"RapidOCR runner 未识别到任何文本。";
    }
    return true;
}

bool run_custom_adapter_ocr(
    HMODULE module,
    const std::filesystem::path& image_path,
    const std::wstring& model_dir,
    const std::wstring& profile,
    int ort_threads,
    std::wstring& out_text,
    std::wstring& out_error) {
    auto fn_init = reinterpret_cast<FnONNXOCRInit>(GetProcAddress(module, "ONNXOCR_Init"));
    auto fn_init_profile =
        reinterpret_cast<FnONNXOCRInitProfile>(GetProcAddress(module, "ONNXOCR_InitProfile"));
    auto fn_recognize = reinterpret_cast<FnONNXOCRRecognize>(GetProcAddress(module, "ONNXOCR_Recognize"));
    auto fn_free = reinterpret_cast<FnONNXOCRFreeResult>(GetProcAddress(module, "ONNXOCR_FreeResult"));
    auto fn_set_threads =
        reinterpret_cast<FnONNXOCRSetThreadCount>(GetProcAddress(module, "ONNXOCR_SetThreadCount"));

    if ((!fn_init && !fn_init_profile) || !fn_recognize || !fn_free) {
        return false;
    }

    if (fn_set_threads) {
        fn_set_threads(std::max(1, ort_threads));
    }

    const bool initialized =
        fn_init_profile ? fn_init_profile(model_dir.c_str(), profile.c_str()) : fn_init(model_dir.c_str());
    if (!initialized) {
        out_error = L"RapidOCR ONNX 模型载入失败。";
        return true;
    }

    wchar_t* result_str = fn_recognize(image_path.c_str());
    if (result_str) {
        out_text = result_str;
        fn_free(result_str);
    } else {
        out_error = L"RapidOCR ONNX 未识别到任何文本。";
    }
    return true;
}

bool run_rapidocr_onnx_c_api(
    HMODULE module,
    const std::filesystem::path& image_path,
    const std::wstring& model_dir,
    int ort_threads,
    std::wstring& out_text,
    std::wstring& out_error) {
    auto fn_init = reinterpret_cast<FnOcrInit>(GetProcAddress(module, "OcrInit"));
    auto fn_detect = reinterpret_cast<FnOcrDetect>(GetProcAddress(module, "OcrDetect"));
    auto fn_get_len = reinterpret_cast<FnOcrGetLen>(GetProcAddress(module, "OcrGetLen"));
    auto fn_get_result = reinterpret_cast<FnOcrGetResult>(GetProcAddress(module, "OcrGetResult"));
    auto fn_destroy = reinterpret_cast<FnOcrDestroy>(GetProcAddress(module, "OcrDestroy"));
    if (!fn_init || !fn_detect || !fn_get_len || !fn_get_result || !fn_destroy) {
        return false;
    }

    const std::filesystem::path model_root(model_dir);
    const std::string det_model = to_utf8((model_root / L"det.onnx").wstring());
    const std::string cls_model = to_utf8((model_root / L"cls.onnx").wstring());
    const std::string rec_model = to_utf8((model_root / L"rec.onnx").wstring());
    const std::string dict = to_utf8((model_root / L"dict.txt").wstring());
    void* handle = fn_init(det_model.c_str(), cls_model.c_str(), rec_model.c_str(), dict.c_str(), std::max(1, ort_threads));
    if (!handle) {
        out_error = L"RapidOcrOnnx C API 模型载入失败。";
        return true;
    }

    bool recognized = false;
    try {
        const std::filesystem::path image_dir_path = image_path.parent_path();
        const std::string image_dir = to_utf8((image_dir_path.wstring() + L"\\"));
        const std::string image_name = to_utf8(image_path.filename().wstring());
        RapidOcrOnnxParam params;
        if (fn_detect(handle, image_dir.c_str(), image_name.c_str(), &params)) {
            const int result_len = fn_get_len(handle);
            if (result_len > 1) {
                std::vector<char> buffer(static_cast<std::size_t>(result_len) + 1, '\0');
                fn_get_result(handle, buffer.data(), static_cast<int>(buffer.size()));
                out_text = from_utf8(buffer.data());
                recognized = !out_text.empty();
            }
        }
    } catch (...) {
        out_error = L"RapidOcrOnnx C API 识别时发生异常。";
    }

    fn_destroy(handle);
    if (!recognized && out_error.empty()) {
        out_error = L"RapidOcrOnnx C API 未识别到任何文本。";
    }
    return true;
}

bool run_onnx_ocr(
    const std::filesystem::path& dll_path,
    const std::filesystem::path& image_path,
    const std::wstring& model_dir,
    const std::wstring& dependency_dir,
    const std::wstring& profile,
    int ort_threads,
    std::wstring& out_text,
    std::wstring& out_error) {
    SetDllDirectoryW(dll_path.parent_path().c_str());
    const std::wstring thread_count = std::to_wstring(std::max(1, ort_threads));
    SetEnvironmentVariableW(L"OMP_NUM_THREADS", thread_count.c_str());
    SetEnvironmentVariableW(L"OMP_WAIT_POLICY", L"PASSIVE");
    SetEnvironmentVariableW(L"AIRSHOT_OCR_MODEL_DIR", model_dir.c_str());
    SetEnvironmentVariableW(L"AIRSHOT_OCR_DEPENDENCY_DIR", dependency_dir.c_str());
    SetEnvironmentVariableW(L"AIRSHOT_OCR_PROFILE", profile.c_str());

    HMODULE module = LoadLibraryW(dll_path.c_str());
    if (!module) {
        out_error = L"无法加载 rapidocr_api.dll (错误码 " + std::to_wstring(GetLastError()) + L")。";
        SetDllDirectoryW(nullptr);
        return false;
    }

    bool handled = run_custom_adapter_ocr(module, image_path, model_dir, profile, ort_threads, out_text, out_error);
    if (!handled) {
        handled = run_rapidocr_onnx_c_api(module, image_path, model_dir, ort_threads, out_text, out_error);
    }
    if (!handled) {
        out_error = L"rapidocr_api.dll 中缺少必要的 RapidOCR 导出函数。";
        FreeLibrary(module);
        SetDllDirectoryW(nullptr);
        return false;
    }

    FreeLibrary(module);
    SetDllDirectoryW(nullptr);
    return !out_text.empty();
}

}  // namespace

namespace airshot {

int run_ocr_cli(std::span<const std::wstring> arguments) {
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    std::wstring engine = L"onnx";
    std::wstring image_path;
    std::wstring model_dir;
    std::wstring dependency_dir;
    std::wstring ocr_profile{std::wstring(airshot::kDefaultOcrEngine)};
    int ort_threads = 2;

    // Loop starts at 1 to skip "--ocr-internal"
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        std::wstring arg = arguments[i];
        if (arg == L"--engine" && i + 1 < arguments.size()) {
            engine = arguments[++i];
        } else if (arg == L"--image" && i + 1 < arguments.size()) {
            image_path = arguments[++i];
        } else if (arg == L"--model-dir" && i + 1 < arguments.size()) {
            model_dir = arguments[++i];
        } else if (arg == L"--dependency-dir" && i + 1 < arguments.size()) {
            dependency_dir = arguments[++i];
        } else if (arg == L"--ocr-profile" && i + 1 < arguments.size()) {
            ocr_profile = airshot::normalize_ocr_engine(arguments[++i]);
        } else if (arg == L"--ort-threads" && i + 1 < arguments.size()) {
            try {
                ort_threads = std::max(1, std::stoi(arguments[++i]));
            } catch (...) {
                ort_threads = 2;
            }
        }
    }

    if (image_path.empty()) {
        std::wcerr << L"错误: 缺少 --image 参数。\n";
        return 1;
    }
    if (engine != L"onnx") {
        std::wcerr << L"错误: OCR 仅支持 RapidOCR ONNX 引擎。\n";
        return 1;
    }

    std::filesystem::path dll_path = get_dependency_file_path(dependency_dir, L"rapidocr_api.dll");
    if (model_dir.empty()) {
        model_dir = (dll_path.parent_path() / L"models" / ocr_profile).wstring();
    }

    std::wstring out_text;
    std::wstring out_error;
    const auto runner_path = get_dependency_file_path(dependency_dir, L"rapidocr_runner.exe");
    bool success = false;
    if (std::filesystem::exists(runner_path)) {
        run_external_runner(
            runner_path,
            image_path,
            model_dir,
            ocr_profile,
            ort_threads,
            out_text,
            out_error);
        success = !out_text.empty();
    } else {
        success = run_onnx_ocr(
            dll_path,
            image_path,
            model_dir,
            dependency_dir,
            ocr_profile,
            ort_threads,
            out_text,
            out_error);
    }

    if (success) {
        std::wcout << out_text;
        return 0;
    }

    std::wcerr << out_error;
    return 2;
}

}  // namespace airshot
