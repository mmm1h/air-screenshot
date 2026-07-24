#include "airshot/common.h"
#include "airshot/config.h"
#include "airshot/ocr.h"
#include "airshot/portable.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD kRunnerTimeoutMs = 120'000;
constexpr std::size_t kMaxRunnerOutputBytes = 8U * 1024U * 1024U;

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE value = value_;
        value_ = nullptr;
        return value;
    }

    void reset(HANDLE value = nullptr) noexcept {
        if (value_ && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_{};
};

std::wstring quote_argument(std::wstring_view value) {
    std::wstring result{L'"'};
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

bool valid_profile(std::wstring_view profile) {
    return profile == airshot::kOcrEngineRapidV5Fast ||
           profile == airshot::kOcrEngineRapidV5Accurate ||
           profile == airshot::kOcrEngineRapidV4Compat;
}

bool regular_non_reparse_file(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

std::optional<std::filesystem::path> normalized_absolute_path(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute =
        std::filesystem::absolute(path, error);
    if (error) {
        return std::nullopt;
    }
    return absolute.lexically_normal();
}

bool paths_equal(
    const std::filesystem::path& first,
    const std::filesystem::path& second) {
    const auto normalized_first =
        normalized_absolute_path(first);
    const auto normalized_second =
        normalized_absolute_path(second);
    if (!normalized_first || !normalized_second) {
        return false;
    }
    const std::wstring first_text =
        normalized_first->generic_wstring();
    const std::wstring second_text =
        normalized_second->generic_wstring();
    return CompareStringOrdinal(
               first_text.c_str(),
               static_cast<int>(first_text.size()),
               second_text.c_str(),
               static_cast<int>(second_text.size()),
               TRUE) == CSTR_EQUAL;
}

std::filesystem::path system_directory() {
    std::wstring buffer(32768, L'\0');
    const UINT size = GetSystemDirectoryW(
        buffer.data(),
        static_cast<UINT>(buffer.size()));
    if (size == 0 || size >= buffer.size()) {
        return {};
    }
    buffer.resize(size);
    return std::filesystem::path(buffer);
}

std::optional<std::wstring> current_dll_directory() {
    SetLastError(ERROR_SUCCESS);
    DWORD required = GetDllDirectoryW(0, nullptr);
    if (required == 0) {
        return GetLastError() == ERROR_SUCCESS
                   ? std::optional<std::wstring>(std::wstring{})
                   : std::nullopt;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        std::wstring directory(required, L'\0');
        SetLastError(ERROR_SUCCESS);
        const DWORD size = GetDllDirectoryW(
            static_cast<DWORD>(directory.size()),
            directory.data());
        if (size == 0) {
            return GetLastError() == ERROR_SUCCESS
                       ? std::optional<std::wstring>(std::wstring{})
                       : std::nullopt;
        }
        if (size < directory.size()) {
            directory.resize(size);
            return directory;
        }
        required = size;
    }
    return std::nullopt;
}

bool equals_ordinal_ignore_case(
    std::wstring_view first,
    std::wstring_view second) {
    return first.size() == second.size() &&
           CompareStringOrdinal(
               first.data(),
               static_cast<int>(first.size()),
               second.data(),
               static_cast<int>(second.size()),
               TRUE) == CSTR_EQUAL;
}

bool starts_with_ordinal_ignore_case(
    std::wstring_view value,
    std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
           CompareStringOrdinal(
               value.data(),
               static_cast<int>(prefix.size()),
               prefix.data(),
               static_cast<int>(prefix.size()),
               TRUE) == CSTR_EQUAL;
}

bool remove_from_runner_environment(std::wstring_view entry) {
    const std::size_t search_from =
        !entry.empty() && entry.front() == L'=' ? 1U : 0U;
    const std::size_t separator = entry.find(L'=', search_from);
    if (separator == std::wstring_view::npos) {
        return false;
    }
    const std::wstring_view name = entry.substr(0, separator);
    return starts_with_ordinal_ignore_case(name, L"PYTHON") ||
           starts_with_ordinal_ignore_case(name, L"_PYI_") ||
           equals_ordinal_ignore_case(name, L"_MEIPASS2") ||
           equals_ordinal_ignore_case(
               name, L"PYINSTALLER_RESET_ENVIRONMENT");
}

std::optional<std::vector<wchar_t>> runner_environment_block() {
    LPWCH raw_environment = GetEnvironmentStringsW();
    if (!raw_environment) {
        return std::nullopt;
    }

    try {
        std::vector<std::wstring> entries;
        for (const wchar_t* cursor = raw_environment;
             *cursor != L'\0';) {
            const std::wstring_view entry(cursor);
            if (!remove_from_runner_environment(entry)) {
                entries.emplace_back(entry);
            }
            cursor += entry.size() + 1U;
        }
        FreeEnvironmentStringsW(raw_environment);
        raw_environment = nullptr;

        entries.emplace_back(L"PYINSTALLER_RESET_ENVIRONMENT=1");
        std::ranges::sort(
            entries,
            [](const std::wstring& first, const std::wstring& second) {
                const int insensitive = CompareStringOrdinal(
                    first.c_str(),
                    static_cast<int>(first.size()),
                    second.c_str(),
                    static_cast<int>(second.size()),
                    TRUE);
                if (insensitive != CSTR_EQUAL) {
                    return insensitive == CSTR_LESS_THAN;
                }
                return CompareStringOrdinal(
                           first.c_str(),
                           static_cast<int>(first.size()),
                           second.c_str(),
                           static_cast<int>(second.size()),
                           FALSE) == CSTR_LESS_THAN;
            });

        std::size_t characters = 1U;
        for (const auto& entry : entries) {
            if (entry.size() >
                std::numeric_limits<std::size_t>::max() -
                    characters - 1U) {
                return std::nullopt;
            }
            characters += entry.size() + 1U;
        }
        std::vector<wchar_t> block;
        block.reserve(characters);
        for (const auto& entry : entries) {
            block.insert(block.end(), entry.begin(), entry.end());
            block.push_back(L'\0');
        }
        block.push_back(L'\0');
        return block;
    } catch (...) {
        if (raw_environment) {
            FreeEnvironmentStringsW(raw_environment);
        }
        return std::nullopt;
    }
}

bool append_available_output(
    HANDLE pipe,
    std::vector<char>& output,
    bool& output_limit_exceeded) {
    std::array<char, 4096> temporary{};
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED;
        }
        if (available == 0) {
            return true;
        }

        DWORD bytes_read = 0;
        const DWORD read_size = std::min<DWORD>(available, static_cast<DWORD>(temporary.size()));
        if (!ReadFile(pipe, temporary.data(), read_size, &bytes_read, nullptr)) {
            const DWORD error = GetLastError();
            return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED;
        }
        if (bytes_read == 0) {
            return true;
        }
        if (bytes_read > kMaxRunnerOutputBytes - std::min(output.size(), kMaxRunnerOutputBytes)) {
            output_limit_exceeded = true;
            return false;
        }
        output.insert(output.end(), temporary.begin(), temporary.begin() + bytes_read);
    }
}

bool configure_kill_on_close_job(HANDLE job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    return SetInformationJobObject(
               job,
               JobObjectExtendedLimitInformation,
               &limits,
               sizeof(limits)) != FALSE;
}

bool run_external_runner(
    const std::filesystem::path& runner_path,
    const std::filesystem::path& image_path,
    const std::filesystem::path& model_dir,
    std::wstring_view profile,
    int ort_threads,
    std::wstring& out_text,
    std::wstring& out_error) {
    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    SECURITY_ATTRIBUTES pipe_security{sizeof(pipe_security), nullptr, TRUE};
    if (!CreatePipe(&raw_read, &raw_write, &pipe_security, 0)) {
        out_error = L"创建 OCR runner 输出管道失败。";
        return false;
    }
    UniqueHandle stdout_read(raw_read);
    UniqueHandle stdout_write(raw_write);
    if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        out_error = L"限制 OCR runner 管道继承失败。";
        return false;
    }
    UniqueHandle stdin_null(CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &pipe_security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!stdin_null.get() || stdin_null.get() == INVALID_HANDLE_VALUE) {
        out_error = L"创建 OCR runner 标准输入句柄失败。";
        return false;
    }

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.get() || !configure_kill_on_close_job(job.get())) {
        out_error = L"创建 OCR runner 作业对象失败。";
        return false;
    }

    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_bytes);
    if (attribute_bytes == 0) {
        out_error = L"初始化 OCR runner 句柄白名单失败。";
        return false;
    }
    std::vector<std::byte> attribute_storage(attribute_bytes);
    auto* attributes =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 2, 0, &attribute_bytes)) {
        out_error = L"初始化 OCR runner 句柄白名单失败。";
        return false;
    }

    HANDLE inherited_handles[] = {stdin_null.get(), stdout_write.get()};
    if (!UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles,
            sizeof(inherited_handles),
            nullptr,
            nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        out_error = L"配置 OCR runner 句柄白名单失败。";
        return false;
    }

    const DWORD64 mitigation_policy =
        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_PREFER_SYSTEM32_ALWAYS_ON;
    if (!UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
            const_cast<DWORD64*>(&mitigation_policy),
            sizeof(mitigation_policy),
            nullptr,
            nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        out_error = L"配置 OCR runner 进程缓解策略失败。";
        return false;
    }

    std::wstring command_line = quote_argument(runner_path.wstring());
    command_line += L" --image " + quote_argument(image_path.wstring());
    command_line += L" --model-dir " + quote_argument(model_dir.wstring());
    command_line += L" --ocr-profile " + quote_argument(profile);
    command_line += L" --ort-threads " + std::to_wstring(std::clamp(ort_threads, 1, 16));

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_null.get();
    startup.StartupInfo.hStdOutput = stdout_write.get();
    startup.StartupInfo.hStdError = stdout_write.get();
    startup.lpAttributeList = attributes;

    auto environment = runner_environment_block();
    if (!environment) {
        DeleteProcThreadAttributeList(attributes);
        out_error = L"无法构造 OCR runner 的隔离环境。";
        return false;
    }
    const auto working_directory =
        system_directory();
    if (working_directory.empty()) {
        DeleteProcThreadAttributeList(attributes);
        out_error = L"无法确定 OCR runner 的安全工作目录。";
        return false;
    }

    const auto inherited_dll_directory = current_dll_directory();
    if (!inherited_dll_directory) {
        DeleteProcThreadAttributeList(attributes);
        out_error = L"无法读取 OCR launcher 的 DLL 搜索目录。";
        return false;
    }
    const bool clear_dll_directory = !inherited_dll_directory->empty();
    if (clear_dll_directory && !SetDllDirectoryW(L"")) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        out_error = L"无法隔离 OCR runner 的 DLL 搜索目录：" +
                    airshot::windows_error_message(error);
        return false;
    }

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        runner_path.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED |
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        environment->data(),
        working_directory.c_str(),
        &startup.StartupInfo,
        &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    const BOOL restored =
        !clear_dll_directory ||
        SetDllDirectoryW(inherited_dll_directory->c_str());
    const DWORD restore_error = restored ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    stdout_write.reset();

    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);
    if (!restored) {
        if (created) {
            TerminateProcess(process_handle.get(), 125);
            WaitForSingleObject(process_handle.get(), 5'000);
        }
        out_error = L"无法恢复 OCR launcher 的 DLL 搜索目录：" +
                    airshot::windows_error_message(restore_error);
        return false;
    }
    if (!created) {
        out_error = L"无法启动 RapidOCR runner：" +
                    airshot::windows_error_message(create_error);
        return false;
    }

    if (!AssignProcessToJobObject(job.get(), process_handle.get())) {
        TerminateProcess(process_handle.get(), 125);
        WaitForSingleObject(process_handle.get(), 5'000);
        out_error = L"无法隔离 RapidOCR runner：" + airshot::windows_error_message(GetLastError());
        return false;
    }
    if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
        TerminateJobObject(job.get(), 125);
        WaitForSingleObject(process_handle.get(), 5'000);
        out_error = L"无法启动 RapidOCR runner 线程。";
        return false;
    }

    std::vector<char> output;
    output.reserve(4096);
    const ULONGLONG deadline = GetTickCount64() + kRunnerTimeoutMs;
    bool timed_out = false;
    bool output_limit_exceeded = false;
    bool wait_failed = false;

    for (;;) {
        if (!append_available_output(stdout_read.get(), output, output_limit_exceeded)) {
            if (output_limit_exceeded) {
                TerminateJobObject(job.get(), 125);
                WaitForSingleObject(process_handle.get(), 5'000);
            }
            break;
        }

        const DWORD wait_result = WaitForSingleObject(process_handle.get(), 25);
        if (wait_result == WAIT_OBJECT_0) {
            append_available_output(stdout_read.get(), output, output_limit_exceeded);
            break;
        }
        if (wait_result == WAIT_FAILED) {
            wait_failed = true;
            TerminateJobObject(job.get(), 125);
            WaitForSingleObject(process_handle.get(), 5'000);
            break;
        }
        if (GetTickCount64() >= deadline) {
            timed_out = true;
            TerminateJobObject(job.get(), 124);
            WaitForSingleObject(process_handle.get(), 5'000);
            break;
        }
    }

    DWORD exit_code = 125;
    GetExitCodeProcess(process_handle.get(), &exit_code);
    job.reset();

    if (timed_out) {
        out_error = L"RapidOCR runner 超过 120 秒，已停止本次识别。";
        return false;
    }
    if (output_limit_exceeded) {
        out_error = L"RapidOCR runner 输出超过 8 MiB，已停止本次识别。";
        return false;
    }
    if (wait_failed) {
        out_error = L"等待 RapidOCR runner 时发生错误。";
        return false;
    }

    const std::string_view output_bytes(output.data(), output.size());
    const std::wstring decoded = airshot::from_utf8(output_bytes);
    if (!output.empty() && decoded.empty()) {
        out_error = L"RapidOCR runner 返回了无效的 UTF-8 输出。";
        return false;
    }
    if (exit_code != 0) {
        out_error = decoded.empty() ? L"RapidOCR runner 执行失败。" : decoded;
        return false;
    }
    if (decoded.empty()) {
        out_error = L"RapidOCR runner 未识别到任何文本。";
        return false;
    }

    out_text = decoded;
    return true;
}

bool write_utf8(FILE* stream, std::wstring_view value) {
    if (value.empty()) {
        return true;
    }
    const std::string bytes = airshot::to_utf8(value);
    return !bytes.empty() &&
           std::fwrite(
               bytes.data(),
               1,
               bytes.size(),
               stream) == bytes.size();
}

}  // namespace

namespace airshot {

int run_ocr_cli(std::span<const std::wstring> arguments) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    std::wstring engine = L"onnx";
    std::wstring image_path;
    std::wstring model_dir;
    std::wstring dependency_dir;
    std::wstring profile{kDefaultOcrEngine};
    int ort_threads = 2;

    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::wstring_view argument = arguments[i];
        auto take_value = [&]() -> const std::wstring* {
            if (i + 1 >= arguments.size()) {
                return nullptr;
            }
            return &arguments[++i];
        };

        const std::wstring* value = nullptr;
        if (argument == L"--engine" && (value = take_value())) {
            engine = *value;
        } else if (argument == L"--image" && (value = take_value())) {
            image_path = *value;
        } else if (argument == L"--model-dir" && (value = take_value())) {
            model_dir = *value;
        } else if (argument == L"--dependency-dir" && (value = take_value())) {
            dependency_dir = *value;
        } else if (argument == L"--ocr-profile" && (value = take_value())) {
            profile = *value;
        } else if (argument == L"--ort-threads" && (value = take_value())) {
            try {
                std::size_t parsed = 0;
                const int requested = std::stoi(*value, &parsed);
                if (parsed != value->size() || requested < 1 || requested > 16) {
                    throw std::invalid_argument("thread count");
                }
                ort_threads = requested;
            } catch (...) {
                write_utf8(stderr, L"错误: --ort-threads 必须是 1 到 16 的整数。\n");
                return 1;
            }
        } else {
            write_utf8(stderr, L"错误: OCR 内部命令参数无效或缺少值。\n");
            return 1;
        }
    }

    if (engine != L"onnx" || image_path.empty() ||
        dependency_dir.empty() || !valid_profile(profile)) {
        write_utf8(stderr, L"错误: OCR 内部命令参数无效。\n");
        return 1;
    }

    const std::filesystem::path dependency_root =
        std::filesystem::path(dependency_dir);
    std::wstring lease_error;
    auto dependency_lease =
        acquire_ocr_dependency_lease(
            dependency_root,
            true,
            true,
            &lease_error);
    if (!dependency_lease) {
        write_utf8(
            stderr,
            L"OCR 依赖安全校验失败：" +
                (lease_error.empty()
                     ? std::wstring(L"未知错误")
                     : lease_error) +
                L"\n");
        return 2;
    }

    const std::filesystem::path verified_root =
        dependency_lease->root();
    const std::filesystem::path runner_path =
        verified_root / L"rapidocr_runner.exe";
    const std::filesystem::path expected_models =
        verified_root / L"models" / profile;
    if (model_dir.empty()) {
        model_dir = expected_models.wstring();
    }

    const std::filesystem::path image(image_path);
    const std::filesystem::path models(model_dir);
    if (!paths_equal(models, expected_models)) {
        write_utf8(
            stderr,
            L"OCR 模型目录与所选依赖 profile 不匹配。\n");
        return 2;
    }
    if (!regular_non_reparse_file(runner_path)) {
        write_utf8(stderr, L"未找到可信的 RapidOCR runner。\n");
        return 2;
    }
    if (!regular_non_reparse_file(image)) {
        write_utf8(stderr, L"OCR 输入图像不存在或是不安全的 reparse point。\n");
        return 2;
    }
    for (const wchar_t* file : {L"det.onnx", L"rec.onnx", L"cls.onnx", L"dict.txt"}) {
        if (!regular_non_reparse_file(models / file)) {
            write_utf8(stderr, L"OCR 模型目录不完整或包含不安全的 reparse point。\n");
            return 2;
        }
    }

    std::wstring text;
    std::wstring error;
    if (!run_external_runner(
            runner_path,
            image,
            models,
            profile,
            ort_threads,
            text,
            error)) {
        write_utf8(stderr, error);
        return 2;
    }

    if (!write_utf8(stdout, text)) {
        return 2;
    }
    return 0;
}

}  // namespace airshot
