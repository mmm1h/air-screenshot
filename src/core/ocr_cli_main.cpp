#include "airshot/common.h"
#include "airshot/clipboard.h"
#include "airshot/config.h"
#include "airshot/ocr.h"
#include "airshot/portable.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <io.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD kRunnerTimeoutMs = 120'000;
constexpr std::size_t kMaxRunnerProtocolBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaxRunnerDiagnosticBytes = 1U * 1024U * 1024U;

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
    std::size_t maximum_bytes,
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
        if (bytes_read > maximum_bytes - std::min(output.size(), maximum_bytes)) {
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
    const airshot::OcrProtocolExpectations& expected,
    int ort_threads,
    std::string& out_protocol,
    std::wstring& out_diagnostic,
    std::wstring& out_error) {
    HANDLE raw_stdout_read = nullptr;
    HANDLE raw_stdout_write = nullptr;
    HANDLE raw_stderr_read = nullptr;
    HANDLE raw_stderr_write = nullptr;
    SECURITY_ATTRIBUTES pipe_security{sizeof(pipe_security), nullptr, TRUE};
    if (!CreatePipe(&raw_stdout_read, &raw_stdout_write, &pipe_security, 0) ||
        !CreatePipe(&raw_stderr_read, &raw_stderr_write, &pipe_security, 0)) {
        if (raw_stdout_read) {
            CloseHandle(raw_stdout_read);
        }
        if (raw_stdout_write) {
            CloseHandle(raw_stdout_write);
        }
        if (raw_stderr_read) {
            CloseHandle(raw_stderr_read);
        }
        if (raw_stderr_write) {
            CloseHandle(raw_stderr_write);
        }
        out_error = L"创建 OCR runner 输出管道失败。";
        return false;
    }
    UniqueHandle stdout_read(raw_stdout_read);
    UniqueHandle stdout_write(raw_stdout_write);
    UniqueHandle stderr_read(raw_stderr_read);
    UniqueHandle stderr_write(raw_stderr_write);
    if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
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

    HANDLE inherited_handles[] = {
        stdin_null.get(), stdout_write.get(), stderr_write.get()};
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
    command_line += L" --ort-threads " + std::to_wstring(std::clamp(ort_threads, 1, 4));
    command_line += L" --source-width " + std::to_wstring(expected.source_width);
    command_line += L" --source-height " + std::to_wstring(expected.source_height);
    command_line += L" --input-width " + std::to_wstring(expected.input_width);
    command_line += L" --input-height " + std::to_wstring(expected.input_height);
    command_line += L" --scale-x " + std::format(L"{:.17g}", expected.scale_x);
    command_line += L" --scale-y " + std::format(L"{:.17g}", expected.scale_y);
    command_line += L" --preprocess-mode " + quote_argument(expected.resample);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_null.get();
    startup.StartupInfo.hStdOutput = stdout_write.get();
    startup.StartupInfo.hStdError = stderr_write.get();
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
    stderr_write.reset();

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

    std::vector<char> protocol_output;
    protocol_output.reserve(4096);
    std::vector<char> diagnostic_output;
    diagnostic_output.reserve(1024);
    const ULONGLONG deadline = GetTickCount64() + kRunnerTimeoutMs;
    bool timed_out = false;
    bool protocol_limit_exceeded = false;
    bool diagnostic_limit_exceeded = false;
    bool wait_failed = false;

    for (;;) {
        const bool stdout_ok = append_available_output(
            stdout_read.get(),
            protocol_output,
            kMaxRunnerProtocolBytes,
            protocol_limit_exceeded);
        const bool stderr_ok = append_available_output(
            stderr_read.get(),
            diagnostic_output,
            kMaxRunnerDiagnosticBytes,
            diagnostic_limit_exceeded);
        if (!stdout_ok || !stderr_ok) {
            if (protocol_limit_exceeded || diagnostic_limit_exceeded) {
                TerminateJobObject(job.get(), 125);
                WaitForSingleObject(process_handle.get(), 5'000);
            }
            break;
        }

        const DWORD wait_result = WaitForSingleObject(process_handle.get(), 25);
        if (wait_result == WAIT_OBJECT_0) {
            append_available_output(
                stdout_read.get(),
                protocol_output,
                kMaxRunnerProtocolBytes,
                protocol_limit_exceeded);
            append_available_output(
                stderr_read.get(),
                diagnostic_output,
                kMaxRunnerDiagnosticBytes,
                diagnostic_limit_exceeded);
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
    if (protocol_limit_exceeded) {
        out_error = L"RapidOCR runner 协议输出超过 8 MiB，已停止本次识别。";
        return false;
    }
    if (diagnostic_limit_exceeded) {
        out_error = L"RapidOCR runner 诊断输出超过 1 MiB，已停止本次识别。";
        return false;
    }
    if (wait_failed) {
        out_error = L"等待 RapidOCR runner 时发生错误。";
        return false;
    }

    const std::string protocol_bytes(protocol_output.begin(), protocol_output.end());
    const std::string diagnostic_bytes(diagnostic_output.begin(), diagnostic_output.end());
    const std::wstring decoded_diagnostic = airshot::from_utf8(diagnostic_bytes);
    if (!diagnostic_output.empty() && decoded_diagnostic.empty()) {
        out_error = L"RapidOCR runner 返回了无效的 UTF-8 诊断信息。";
        return false;
    }
    if (exit_code != 0) {
        out_error = decoded_diagnostic.empty()
                        ? L"RapidOCR runner 执行失败。"
                        : decoded_diagnostic;
        return false;
    }
    if (protocol_bytes.empty()) {
        out_error = L"RapidOCR runner 未返回协议数据。";
        return false;
    }
    std::wstring protocol_error;
    if (!airshot::parse_ocr_runner_protocol(
            protocol_bytes,
            expected,
            &protocol_error)) {
        out_error = L"RapidOCR runner 协议无效：" + protocol_error;
        return false;
    }

    out_protocol = protocol_bytes;
    out_diagnostic = decoded_diagnostic;
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

bool write_bytes(FILE* stream, std::string_view value) {
    return value.empty() ||
           std::fwrite(value.data(), 1, value.size(), stream) == value.size();
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
    int source_width = 0;
    int source_height = 0;
    int input_width = 0;
    int input_height = 0;
    double scale_x = 0.0;
    double scale_y = 0.0;
    std::wstring preprocess_mode;

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
                if (parsed != value->size() || requested < 1 || requested > 4) {
                    throw std::invalid_argument("thread count");
                }
                ort_threads = requested;
            } catch (...) {
                write_utf8(stderr, L"错误: --ort-threads 必须是 1 到 4 的整数。\n");
                return 1;
            }
        } else if ((argument == L"--source-width" ||
                    argument == L"--source-height" ||
                    argument == L"--input-width" ||
                    argument == L"--input-height") &&
                   (value = take_value())) {
            try {
                std::size_t parsed = 0;
                const int requested = std::stoi(*value, &parsed);
                if (parsed != value->size() || requested < 1 || requested > 100'000) {
                    throw std::invalid_argument("dimension");
                }
                if (argument == L"--source-width") {
                    source_width = requested;
                } else if (argument == L"--source-height") {
                    source_height = requested;
                } else if (argument == L"--input-width") {
                    input_width = requested;
                } else {
                    input_height = requested;
                }
            } catch (...) {
                write_utf8(stderr, L"错误: OCR 图像尺寸参数无效。\n");
                return 1;
            }
        } else if ((argument == L"--scale-x" || argument == L"--scale-y") &&
                   (value = take_value())) {
            try {
                std::size_t parsed = 0;
                const double requested = std::stod(*value, &parsed);
                if (parsed != value->size() || !std::isfinite(requested) ||
                    requested < 0.01 || requested > 2.0) {
                    throw std::invalid_argument("scale");
                }
                if (argument == L"--scale-x") {
                    scale_x = requested;
                } else {
                    scale_y = requested;
                }
            } catch (...) {
                write_utf8(stderr, L"错误: OCR 缩放比例参数无效。\n");
                return 1;
            }
        } else if (argument == L"--preprocess-mode" && (value = take_value())) {
            preprocess_mode = *value;
        } else {
            write_utf8(stderr, L"错误: OCR 内部命令参数无效或缺少值。\n");
            return 1;
        }
    }

    const bool valid_preprocess_mode =
        preprocess_mode == L"none" ||
        preprocess_mode == L"bilinear-upscale" ||
        preprocess_mode == L"progressive-bilinear-downscale";
    if (engine != L"onnx" || image_path.empty() ||
        dependency_dir.empty() || !valid_profile(profile) ||
        source_width <= 0 || source_height <= 0 ||
        input_width <= 0 || input_height <= 0 ||
        scale_x <= 0.0 || scale_y <= 0.0 || !valid_preprocess_mode ||
        std::abs(scale_x - static_cast<double>(input_width) / source_width) > 1.0e-9 ||
        std::abs(scale_y - static_cast<double>(input_height) / source_height) > 1.0e-9) {
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

    const OcrProtocolExpectations expected{
        profile,
        source_width,
        source_height,
        input_width,
        input_height,
        scale_x,
        scale_y,
        preprocess_mode,
    };
    std::string protocol;
    std::wstring diagnostic;
    std::wstring error;
    if (!run_external_runner(
            runner_path,
            image,
            models,
            profile,
            expected,
            ort_threads,
            protocol,
            diagnostic,
            error)) {
        write_utf8(stderr, error);
        return 2;
    }

    if (!diagnostic.empty()) {
        write_utf8(stderr, diagnostic);
    }
    if (!write_bytes(stdout, protocol)) {
        return 2;
    }
    return 0;
}

int run_ocr_warm_smoke(std::span<const std::wstring> arguments) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    if (arguments.size() != 5 || arguments[0] != L"--ocr-warm-smoke" ||
        arguments[1] != L"--image" || arguments[3] != L"--ocr-profile" ||
        arguments[2].empty() || !valid_profile(arguments[4])) {
        write_utf8(stderr, L"错误: OCR 热进程烟测参数无效。\n");
        return 2;
    }

    std::wstring decode_error;
    const auto bitmap = decode_local_image_file(
        std::filesystem::path(arguments[2]),
        &decode_error);
    if (!bitmap) {
        write_utf8(
            stderr,
            L"OCR 热进程烟测无法解码图片：" + decode_error + L"\n");
        return 2;
    }

    AppConfig config;
    config.ocr_engine = arguments[4];
    const OcrOutput first = recognize_text(*bitmap, config);
    if (!first.ok) {
        write_utf8(
            stderr,
            L"OCR 热进程首次识别失败：" + first.error + L"\n");
        return 2;
    }
    const OcrOutput second = recognize_text(*bitmap, config);
    if (!second.ok) {
        write_utf8(
            stderr,
            L"OCR 热进程复用识别失败：" + second.error + L"\n");
        return 2;
    }

    const bool valid_outputs =
        !first.text.empty() && first.text == second.text &&
        !first.blocks.empty() && !second.blocks.empty() &&
        first.profile == arguments[4] && second.profile == arguments[4] &&
        first.preprocess.source_width == bitmap->width &&
        first.preprocess.source_height == bitmap->height &&
        second.preprocess.source_width == bitmap->width &&
        second.preprocess.source_height == bitmap->height &&
        first.timings.model_init_ms > 0.0 &&
        second.timings.model_init_ms == 0.0 &&
        first.timings.total_ms > 0.0 && second.timings.total_ms > 0.0;
    if (!valid_outputs) {
        write_utf8(
            stderr,
            std::format(
                L"OCR 热进程未复用生产识别链路（初始化耗时：{:.3f} / {:.3f} ms）。\n",
                first.timings.model_init_ms,
                second.timings.model_init_ms));
        return 2;
    }

    if (!write_utf8(stdout, second.text + L"\n")) {
        return 2;
    }
    return 0;
}

}  // namespace airshot
