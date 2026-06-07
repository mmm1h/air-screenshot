#include "airshot/ocr.h"
#include "airshot/output.h"
#include "airshot/config.h"
#include "airshot/portable.h"

#include <windows.h>
#include <urlmon.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <string>
#include <format>
#include <vector>

namespace airshot {
namespace {

constexpr DWORD kOcrProcessTimeoutMs = 90'000;
constexpr int kMaxOcrImageEdge = 4096;
constexpr std::uint64_t kMaxOcrPixels = 8ULL * 1024ULL * 1024ULL;

struct OcrEngineSpec {
    std::wstring_view id;
    std::wstring_view label;
    std::wstring_view profile_directory;
};

constexpr std::array<OcrEngineSpec, 3> kOcrEngineSpecs{{
    {kOcrEngineRapidV5Fast, L"极速 OCR", L"models/rapidocr-v5-fast"},
    {kOcrEngineRapidV5Accurate, L"高精度 OCR", L"models/rapidocr-v5-accurate"},
    {kOcrEngineRapidV4Compat, L"兼容 OCR", L"models/rapidocr-v4-compat"},
}};

constexpr std::array<std::wstring_view, 3> kRapidOcrCommonRequiredFiles{
    L"rapidocr_api.dll",
    L"onnxruntime.dll",
    L"rapidocr_runner.exe",
};

constexpr std::array<std::wstring_view, 4> kRapidOcrProfileRequiredFiles{
    L"det.onnx",
    L"rec.onnx",
    L"cls.onnx",
    L"dict.txt",
};

std::wstring normalized_hex(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character >= L'a' && character <= L'f') {
            result.push_back(static_cast<wchar_t>(character - L'a' + L'A'));
        } else if ((character >= L'A' && character <= L'F') || (character >= L'0' && character <= L'9')) {
            result.push_back(character);
        } else {
            return {};
        }
    }
    return result;
}

bool is_zero_hash(std::wstring_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](wchar_t ch) { return ch == L'0'; });
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

const OcrEngineSpec& ocr_engine_spec(std::wstring_view engine) {
    const std::wstring normalized = normalize_ocr_engine(engine);
    for (const auto& spec : kOcrEngineSpecs) {
        if (normalized == spec.id) {
            return spec;
        }
    }
    return kOcrEngineSpecs.front();
}

std::filesystem::path executable_directory() {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    return std::filesystem::path(exe_path).parent_path();
}

std::vector<std::filesystem::path> ocr_dependency_roots() {
    std::vector<std::filesystem::path> roots;
    roots.push_back(config_directory() / L"ocr" / kRapidOcrOnnxPackageId);
    roots.push_back(executable_directory() / L"ocr" / kRapidOcrOnnxPackageId);
    return roots;
}

bool file_exists_in_directory(const std::filesystem::path& root, std::wstring_view relative_path) {
    return std::filesystem::exists(root / std::wstring(relative_path));
}

std::wstring profile_relative_path(const OcrEngineSpec& spec, std::wstring_view file) {
    std::wstring result(spec.profile_directory);
    result += L"/";
    result += file;
    return result;
}

std::vector<std::wstring> required_dependency_files(const OcrEngineSpec& spec) {
    std::vector<std::wstring> files;
    for (const auto file : kRapidOcrCommonRequiredFiles) {
        files.emplace_back(file);
    }
    for (const auto file : kRapidOcrProfileRequiredFiles) {
        files.push_back(profile_relative_path(spec, file));
    }
    return files;
}

std::vector<std::wstring> all_required_dependency_files() {
    std::vector<std::wstring> files;
    for (const auto file : kRapidOcrCommonRequiredFiles) {
        files.emplace_back(file);
    }
    for (const auto& spec : kOcrEngineSpecs) {
        for (const auto file : kRapidOcrProfileRequiredFiles) {
            files.push_back(profile_relative_path(spec, file));
        }
    }
    return files;
}

bool dependency_directory_ready(
    const std::filesystem::path& root,
    const OcrEngineSpec& spec,
    std::wstring* missing_file = nullptr) {
    for (const auto& file : required_dependency_files(spec)) {
        if (!file_exists_in_directory(root, file)) {
            if (missing_file) {
                *missing_file = file;
            }
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> find_rapid_ocr_dependency_directory(const OcrEngineSpec& spec) {
    for (const auto& root : ocr_dependency_roots()) {
        if (dependency_directory_ready(root, spec)) {
            return root;
        }
    }
    return std::nullopt;
}

bool valid_manifest_relative_path(std::wstring_view value) {
    if (value.empty() || value.find(L":") != std::wstring_view::npos ||
        value.starts_with(L"\\") || value.starts_with(L"/") ||
        value.ends_with(L"\\") || value.ends_with(L"/")) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t slash = value.find_first_of(L"\\/", start);
        const std::wstring_view part =
            value.substr(start, slash == std::wstring_view::npos ? value.size() - start : slash - start);
        if (part.empty() || part == L"." || part == L"..") {
            return false;
        }
        if (slash == std::wstring_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool manifest_contains_required_files(const OcrDependencyManifest& manifest) {
    for (const auto& required : all_required_dependency_files()) {
        const auto found = std::ranges::find_if(manifest.files, [&required](const OcrDependencyFile& file) {
            return file.path == required;
        });
        if (found == manifest.files.end()) {
            return false;
        }
    }
    return true;
}

std::optional<std::wstring> read_text_file(const std::filesystem::path& path, std::wstring* error = nullptr) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            if (error) {
                *error = L"无法读取文件。";
            }
            return std::nullopt;
        }
        const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        return from_utf8(bytes);
    } catch (const std::exception& exception) {
        if (error) {
            *error = from_utf8(exception.what());
        }
        return std::nullopt;
    }
}

bool download_file(std::wstring_view url, const std::filesystem::path& path, std::wstring* error) {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::filesystem::remove(path, ignored);
    const HRESULT result = URLDownloadToFileW(nullptr, std::wstring(url).c_str(), path.c_str(), 0, nullptr);
    if (FAILED(result)) {
        if (error) {
            *error = std::format(L"下载失败：0x{:08X}", static_cast<unsigned int>(result));
        }
        return false;
    }
    return true;
}

bool verify_dependency_file(const OcrDependencyFile& file, const std::filesystem::path& path, std::wstring* error) {
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        if (error) {
            *error = L"无法读取依赖文件大小。";
        }
        return false;
    }
    if (file.size == 0 || size != file.size) {
        if (error) {
            *error = L"OCR 依赖文件大小与清单不一致。";
        }
        return false;
    }
    std::wstring hash_error;
    const std::wstring actual_hash = sha256_file(path, &hash_error);
    if (actual_hash != normalized_hex(file.sha256)) {
        if (error) {
            *error = hash_error.empty() ? L"OCR 依赖文件 SHA256 校验失败。" : hash_error;
        }
        return false;
    }
    return true;
}

Bitmap resize_bitmap_for_ocr(const Bitmap& source) {
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(std::max(0, source.width)) * static_cast<std::uint64_t>(std::max(0, source.height));
    if (source.empty() ||
        (source.width <= kMaxOcrImageEdge && source.height <= kMaxOcrImageEdge && pixels <= kMaxOcrPixels)) {
        return source;
    }

    const double edge_scale = std::min(static_cast<double>(kMaxOcrImageEdge) / source.width,
                                       static_cast<double>(kMaxOcrImageEdge) / source.height);
    const double pixel_scale = std::sqrt(static_cast<double>(kMaxOcrPixels) / static_cast<double>(pixels));
    const double scale = std::min(edge_scale, pixel_scale);
    const int target_width = std::max(1, static_cast<int>(std::floor(source.width * scale)));
    const int target_height = std::max(1, static_cast<int>(std::floor(source.height * scale)));

    Bitmap target(target_width, target_height);
    for (int y = 0; y < target_height; ++y) {
        const int source_y = std::min(source.height - 1, static_cast<int>(static_cast<double>(y) / scale));
        const auto* source_row = source.row(source_y).data();
        auto* target_row = target.row(y).data();
        for (int x = 0; x < target_width; ++x) {
            const int source_x = std::min(source.width - 1, static_cast<int>(static_cast<double>(x) / scale));
            std::memcpy(target_row + x * 4, source_row + source_x * 4, 4);
        }
    }
    return target;
}

OcrOutput run_ocr_process(const std::wstring& cmd_line) {
    HANDLE h_child_stdout_rd = nullptr;
    HANDLE h_child_stdout_wr = nullptr;
    
    SECURITY_ATTRIBUTES sa_attr;
    sa_attr.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa_attr.bInheritHandle = TRUE;
    sa_attr.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&h_child_stdout_rd, &h_child_stdout_wr, &sa_attr, 0)) {
        return {false, {}, L"创建 IPC 管道失败。"};
    }

    if (!SetHandleInformation(h_child_stdout_rd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(h_child_stdout_rd);
        CloseHandle(h_child_stdout_wr);
        return {false, {}, L"设置管道句柄继承失败。"};
    }

    STARTUPINFOW si{};
    si.cb = sizeof(STARTUPINFO);
    si.hStdOutput = h_child_stdout_wr;
    si.hStdError = h_child_stdout_wr;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    std::wstring cmd_copy = cmd_line;

    BOOL success = CreateProcessW(
        nullptr,
        cmd_copy.data(),
        nullptr,
        nullptr,
        TRUE, // Inherit handles
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    CloseHandle(h_child_stdout_wr);

    if (!success) {
        CloseHandle(h_child_stdout_rd);
        return {false, {}, L"无法启动 airshot_ocr.exe 辅助进程。"};
    }

    std::vector<char> buffer;
    char temp_buf[4096];
    const ULONGLONG deadline = GetTickCount64() + kOcrProcessTimeoutMs;
    bool timed_out = false;

    while (true) {
        DWORD available = 0;
        if (PeekNamedPipe(h_child_stdout_rd, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD bytes_read = 0;
            const DWORD read_size = std::min<DWORD>(available, sizeof(temp_buf));
            if (ReadFile(h_child_stdout_rd, temp_buf, read_size, &bytes_read, nullptr) && bytes_read > 0) {
                buffer.insert(buffer.end(), temp_buf, temp_buf + bytes_read);
            }
        }

        const DWORD wait_result = WaitForSingleObject(pi.hProcess, 25);
        if (wait_result == WAIT_OBJECT_0) {
            while (PeekNamedPipe(h_child_stdout_rd, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                DWORD bytes_read = 0;
                const DWORD read_size = std::min<DWORD>(available, sizeof(temp_buf));
                if (!ReadFile(h_child_stdout_rd, temp_buf, read_size, &bytes_read, nullptr) || bytes_read == 0) {
                    break;
                }
                buffer.insert(buffer.end(), temp_buf, temp_buf + bytes_read);
            }
            break;
        }
        if (GetTickCount64() >= deadline) {
            timed_out = true;
            TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
    }

    CloseHandle(h_child_stdout_rd);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (timed_out) {
        return {false, {}, L"OCR 子进程超时，已停止本次识别。"};
    }

    if (exit_code != 0) {
        std::wstring err_msg(reinterpret_cast<const wchar_t*>(buffer.data()), buffer.size() / sizeof(wchar_t));
        return {false, {}, err_msg.empty() ? L"OCR 子进程执行失败。" : err_msg};
    }

    std::wstring result_text(reinterpret_cast<const wchar_t*>(buffer.data()), buffer.size() / sizeof(wchar_t));
    return {true, result_text, {}};
}

}  // namespace

std::filesystem::path rapid_ocr_dependency_directory() {
    return config_directory() / L"ocr" / kRapidOcrOnnxPackageId;
}

std::optional<OcrDependencyManifest> parse_ocr_dependency_manifest(std::wstring_view json) {
    try {
        const ScopedWinrtApartment apartment;
        const auto root = winrt::Windows::Data::Json::JsonObject::Parse(json);
        OcrDependencyManifest manifest;
        manifest.package_id = root.GetNamedString(L"packageId").c_str();
        if (manifest.package_id != kRapidOcrOnnxPackageId) {
            return std::nullopt;
        }

        const auto files = root.GetNamedArray(L"files");
        if (files.Size() == 0 || files.Size() > 512) {
            return std::nullopt;
        }

        for (const auto& value : files) {
            const auto object = value.GetObject();
            OcrDependencyFile file;
            file.path = object.GetNamedString(L"path").c_str();
            file.url = object.GetNamedString(L"url").c_str();
            file.sha256 = normalized_hex(object.GetNamedString(L"sha256").c_str());
            file.size = object.HasKey(L"size") ? static_cast<std::uint64_t>(object.GetNamedNumber(L"size")) : 0;

            if (!valid_manifest_relative_path(file.path) || !file.url.starts_with(L"https://") ||
                file.sha256.size() != 64 || is_zero_hash(file.sha256) ||
                file.size == 0 || file.size > 1024ULL * 1024ULL * 1024ULL) {
                return std::nullopt;
            }
            manifest.files.push_back(std::move(file));
        }

        if (!manifest_contains_required_files(manifest)) {
            return std::nullopt;
        }
        return manifest;
    } catch (...) {
        return std::nullopt;
    }
}

OcrDependencyStatus ocr_dependency_status(std::wstring_view engine) {
    const auto& spec = ocr_engine_spec(engine);
    std::wstring first_missing;
    bool saw_partial_directory = false;
    for (const auto& root : ocr_dependency_roots()) {
        std::wstring missing;
        if (dependency_directory_ready(root, spec, &missing)) {
            return {true, false, L"状态: " + std::wstring(spec.label) + L" 就绪"};
        }
        if (!saw_partial_directory && std::filesystem::exists(root)) {
            saw_partial_directory = true;
            first_missing = missing;
        }
    }
    if (saw_partial_directory) {
        return {false, true, L"状态: 缺少 " + first_missing};
    }
    return {false, true, L"状态: 未下载 RapidOCR ONNX 依赖"};
}

bool download_ocr_dependencies(
    std::wstring_view manifest_url,
    const std::function<void(int)>& progress_callback,
    std::wstring* error) {
    if (!manifest_url.starts_with(L"https://")) {
        if (error) {
            *error = L"OCR 依赖清单地址必须使用 HTTPS。";
        }
        return false;
    }

    if (progress_callback) {
        progress_callback(0);
    }

    const auto root = config_directory() / L"ocr";
    const auto manifest_path = root / L"ocr-dependencies.json.tmp";
    std::wstring download_error;
    if (!download_file(manifest_url, manifest_path, &download_error)) {
        if (error) {
            *error = L"下载 OCR 依赖清单失败：" + download_error;
        }
        return false;
    }
    if (progress_callback) {
        progress_callback(5);
    }

    const auto manifest_text = read_text_file(manifest_path, error);
    std::error_code ignored;
    std::filesystem::remove(manifest_path, ignored);
    if (!manifest_text) {
        return false;
    }
    const auto manifest = parse_ocr_dependency_manifest(*manifest_text);
    if (!manifest) {
        if (error) {
            *error = L"OCR 依赖清单格式无效或缺少必要文件。";
        }
        return false;
    }

    const auto final_dir = rapid_ocr_dependency_directory();
    const auto staging_dir = root / (std::wstring(kRapidOcrOnnxPackageId) + L".download");
    std::filesystem::remove_all(staging_dir, ignored);
    std::filesystem::create_directories(staging_dir, ignored);

    for (std::size_t i = 0; i < manifest->files.size(); ++i) {
        const auto& file = manifest->files[i];
        const auto target = staging_dir / file.path;
        const auto temporary = target.parent_path() / (target.filename().wstring() + L".download");

        if (!download_file(file.url, temporary, error) || !verify_dependency_file(file, temporary, error)) {
            std::filesystem::remove_all(staging_dir, ignored);
            return false;
        }
        std::filesystem::create_directories(target.parent_path(), ignored);
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (error) {
                *error = L"无法写入 OCR 依赖文件：" + windows_error_message(GetLastError());
            }
            std::filesystem::remove_all(staging_dir, ignored);
            return false;
        }
        if (progress_callback) {
            const int progress = 5 + static_cast<int>(((i + 1) * 90) / manifest->files.size());
            progress_callback(std::clamp(progress, 5, 95));
        }
    }

    std::filesystem::remove_all(final_dir, ignored);
    std::filesystem::create_directories(root, ignored);
    if (!MoveFileExW(staging_dir.c_str(), final_dir.c_str(), MOVEFILE_WRITE_THROUGH)) {
        if (error) {
            *error = L"无法安装 OCR 依赖：" + windows_error_message(GetLastError());
        }
        std::filesystem::remove_all(staging_dir, ignored);
        return false;
    }

    if (progress_callback) {
        progress_callback(100);
    }
    return true;
}

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

OcrOutput recognize_text(const Bitmap& bitmap, const AppConfig& config) {
    if (bitmap.empty()) {
        return {false, {}, L"OCR 图像为空。"};
    }

    const auto& spec = ocr_engine_spec(config.ocr_engine);
    const auto runtime_dir = find_rapid_ocr_dependency_directory(spec);
    if (!runtime_dir) {
        const OcrDependencyStatus status = ocr_dependency_status(spec.id);
        return {false, {}, status.message + L"，请在设置中点击“下载依赖”。"};
    }

    const Bitmap ocr_bitmap = resize_bitmap_for_ocr(bitmap);

    std::filesystem::path temp_png = config_directory() / std::format(L"ocr_temp_{}_{}.png", GetCurrentProcessId(), GetCurrentThreadId());
    std::wstring save_error;
    if (!save_png(ocr_bitmap, temp_png, &save_error)) {
        return {false, {}, L"无法保存临时 OCR 选区图像: " + save_error};
    }

    std::filesystem::path ocr_exe = portable_executable_path();

    if (!std::filesystem::exists(ocr_exe)) {
        std::filesystem::remove(temp_png);
        return {false, {}, L"未找到 " + ocr_exe.filename().wstring() + L"，请重新编译或安装程序。"};
    }

    std::wstring cmd_line = quote_argument(ocr_exe.wstring());
    cmd_line += L" --ocr-internal --engine onnx --image " + quote_argument(temp_png.wstring());
    cmd_line += L" --model-dir " + quote_argument((*runtime_dir / std::wstring(spec.profile_directory)).wstring());
    cmd_line += L" --dependency-dir " + quote_argument(runtime_dir->wstring());
    cmd_line += L" --ocr-profile " + quote_argument(std::wstring(spec.id));
    cmd_line += L" --ort-threads 2";

    OcrOutput result = run_ocr_process(cmd_line);

    std::error_code ignored_ec;
    std::filesystem::remove(temp_png, ignored_ec);

    return result;
}

}  // namespace airshot
