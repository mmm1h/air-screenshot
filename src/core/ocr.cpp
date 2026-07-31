#include "airshot/ocr.h"

#include "airshot/config.h"
#include "airshot/output.h"
#include "airshot/portable.h"
#include "ocr_test_support.h"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <objbase.h>
#include <sddl.h>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef AIRSHOT_OCR_MANIFEST_KEY_ID
#define AIRSHOT_OCR_MANIFEST_KEY_ID ""
#endif

#ifndef AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX
#define AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX ""
#endif

#ifndef AIRSHOT_OCR_MIN_SEQUENCE
#define AIRSHOT_OCR_MIN_SEQUENCE 1
#endif

namespace airshot {
namespace {

constexpr DWORD kOcrProcessTimeoutMs = 120'000;
constexpr std::size_t kMaxOcrProcessOutputBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaxOcrProcessDiagnosticBytes = 1U * 1024U * 1024U;
constexpr std::size_t kMaxWarmWorkerRequestBytes = 64U * 1024U;
constexpr std::size_t kMaxWarmWorkerResponseBytes = 8U * 1024U * 1024U;
constexpr std::uint64_t kWarmWorkerSchemaVersion = 1;
constexpr DWORD kWarmWorkerCancelWaitMs = 200;
constexpr std::uint64_t kMaxManifestBytes = 4U * 1024U * 1024U;
constexpr std::uint64_t kMaxDependencyFileBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxDependencyPackageBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxManifestFiles = 2'048;
constexpr std::size_t kMaxDependencyDirectories = 2'048;
constexpr std::uint64_t kManifestSchemaVersion = 1;
constexpr std::uint64_t kMaxSafeJsonInteger = 9'007'199'254'740'991ULL;
constexpr std::uint64_t kCompiledMinimumSequence =
    static_cast<std::uint64_t>(AIRSHOT_OCR_MIN_SEQUENCE);
static_assert(
    kCompiledMinimumSequence >= 1 &&
    kCompiledMinimumSequence <= kMaxSafeJsonInteger);
constexpr std::uint64_t kMinimumManifestTimestamp = 1'577'836'800ULL;
constexpr std::uint64_t kMaximumManifestTimestamp = 4'102'444'800ULL;
constexpr std::uint64_t kMaximumManifestLifetimeSeconds =
    366ULL * 24ULL * 60ULL * 60ULL;
constexpr std::uint64_t kManifestClockSkewSeconds = 5ULL * 60ULL;
constexpr DWORD kDownloadPollTimeoutMs = 1'000;
constexpr int kMaxDownloadAttempts = 8;
constexpr ULONGLONG kDownloadFileDeadlineMs = 10ULL * 60ULL * 1'000ULL;
constexpr ULONGLONG kDownloadOperationDeadlineMs = 30ULL * 60ULL * 1'000ULL;
constexpr int kMaxOcrImageEdge = 32'768;
constexpr std::uint64_t kMaxOcrPixels = 32ULL * 1024ULL * 1024ULL;
constexpr int kMaxOcrProtocolDimension = 100'000;
constexpr std::size_t kMaxOcrProtocolBlocks = 16'384;
constexpr std::size_t kMaxOcrBlockTextCharacters = 4'096;
constexpr std::size_t kMaxOcrTotalTextCharacters = 2U * 1024U * 1024U;
constexpr double kMaxOcrProtocolTimingMs = 120'000.0;
constexpr std::wstring_view kInstalledManifestName = L".airshot-manifest.json";
constexpr std::wstring_view kInstalledSignatureName = L".airshot-manifest.sig";
constexpr std::wstring_view kSequenceHighWatermarkName =
    L".ocr-sequence-high-watermark";
constexpr wchar_t kSequenceMutexName[] =
    L"Local\\AirScreenshot.OcrSequence.v1";
constexpr wchar_t kInstallMutexName[] =
    L"Local\\AirScreenshot.OcrInstall.v1";
constexpr DWORD kOcrMutexWaitMs = 30'000;

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

constexpr std::array<std::wstring_view, 1> kRapidOcrCommonRequiredFiles{
    L"rapidocr_runner.exe",
};

constexpr std::array<std::wstring_view, 4> kRapidOcrProfileRequiredFiles{
    L"det.onnx",
    L"rec.onnx",
    L"cls.onnx",
    L"dict.txt",
};

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
        const HANDLE result = value_;
        value_ = nullptr;
        return result;
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

class LocalMemory {
public:
    LocalMemory() = default;
    explicit LocalMemory(HLOCAL value) noexcept : value_(value) {}
    ~LocalMemory() {
        reset();
    }

    LocalMemory(const LocalMemory&) = delete;
    LocalMemory& operator=(const LocalMemory&) = delete;
    LocalMemory(LocalMemory&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}
    LocalMemory& operator=(LocalMemory&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HLOCAL get() const noexcept {
        return value_;
    }

    void reset(HLOCAL value = nullptr) noexcept {
        if (value_) {
            LocalFree(value_);
        }
        value_ = value;
    }

private:
    HLOCAL value_{};
};

class MutexLease {
public:
    MutexLease() = default;
    explicit MutexLease(HANDLE value) noexcept : handle_(value), owns_(true) {}
    ~MutexLease() {
        reset();
    }

    MutexLease(const MutexLease&) = delete;
    MutexLease& operator=(const MutexLease&) = delete;
    MutexLease(MutexLease&& other) noexcept
        : handle_(std::move(other.handle_)),
          owns_(std::exchange(other.owns_, false)) {}
    MutexLease& operator=(MutexLease&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::move(other.handle_);
            owns_ = std::exchange(other.owns_, false);
        }
        return *this;
    }

private:
    void reset() noexcept {
        if (owns_) {
            ReleaseMutex(handle_.get());
            owns_ = false;
        }
        handle_.reset();
    }

    UniqueHandle handle_;
    bool owns_{};
};

bool ocr_stop_requested(
    std::stop_token stop_token,
    std::wstring* error) {
    if (!stop_token.stop_requested()) {
        return false;
    }
    if (error) {
        *error = L"OCR 已取消。";
    }
    return true;
}

std::optional<MutexLease> acquire_named_mutex(
    const wchar_t* name,
    std::wstring* error,
    std::stop_token stop_token = {}) {
    UniqueHandle mutex(CreateMutexW(nullptr, FALSE, name));
    if (!mutex.get()) {
        if (error) {
            *error =
                L"无法创建 OCR 状态互斥锁：" +
                windows_error_message(GetLastError());
        }
        return std::nullopt;
    }

    const ULONGLONG deadline =
        GetTickCount64() + kOcrMutexWaitMs;
    for (;;) {
        if (ocr_stop_requested(stop_token, error)) {
            return std::nullopt;
        }
        const ULONGLONG now = GetTickCount64();
        const DWORD wait_ms = static_cast<DWORD>(std::min<ULONGLONG>(
            25,
            deadline > now ? deadline - now : 0));
        const DWORD wait_result =
            WaitForSingleObject(mutex.get(), wait_ms);
        if (wait_result == WAIT_OBJECT_0 ||
            wait_result == WAIT_ABANDONED) {
            return MutexLease(mutex.release());
        }
        if (wait_result != WAIT_TIMEOUT ||
            GetTickCount64() >= deadline) {
            if (error) {
                *error =
                    wait_result == WAIT_TIMEOUT
                        ? L"等待 OCR 状态互斥锁超时。"
                        : L"等待 OCR 状态互斥锁失败：" +
                              windows_error_message(GetLastError());
            }
            return std::nullopt;
        }
    }
}

class UniqueWinHttpHandle {
public:
    UniqueWinHttpHandle() = default;
    explicit UniqueWinHttpHandle(HINTERNET value) noexcept : value_(value) {}
    ~UniqueWinHttpHandle() {
        reset();
    }

    UniqueWinHttpHandle(const UniqueWinHttpHandle&) = delete;
    UniqueWinHttpHandle& operator=(const UniqueWinHttpHandle&) = delete;

    UniqueWinHttpHandle(UniqueWinHttpHandle&& other) noexcept
        : value_(other.release()) {}
    UniqueWinHttpHandle& operator=(UniqueWinHttpHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HINTERNET get() const noexcept {
        return value_;
    }

    [[nodiscard]] HINTERNET release() noexcept {
        const HINTERNET result = value_;
        value_ = nullptr;
        return result;
    }

    void reset(HINTERNET value = nullptr) noexcept {
        if (value_) {
            WinHttpCloseHandle(value_);
        }
        value_ = value;
    }

private:
    HINTERNET value_{};
};

class WinHttpDownloadContext {
public:
    bool initialize(std::wstring* error) {
        session_.reset(WinHttpOpen(
            L"AirScreenshot OCR/1",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0));
        if (!session_.get() ||
            !WinHttpSetTimeouts(
                session_.get(),
                kDownloadPollTimeoutMs,
                kDownloadPollTimeoutMs,
                kDownloadPollTimeoutMs,
                kDownloadPollTimeoutMs)) {
            if (error) {
                *error =
                    L"无法初始化 OCR WinHTTP 会话：" +
                    windows_error_message(GetLastError());
            }
            return false;
        }
        return true;
    }

    HINTERNET connection(
        std::wstring_view host,
        INTERNET_PORT port,
        std::wstring* error) {
        std::wstring normalized_host(host);
        std::ranges::transform(
            normalized_host,
            normalized_host.begin(),
            [](wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
        const auto existing = std::ranges::find_if(
            connections_,
            [&normalized_host, port](const Connection& candidate) {
                return candidate.host == normalized_host &&
                       candidate.port == port;
            });
        if (existing != connections_.end()) {
            return existing->handle.get();
        }

        UniqueWinHttpHandle handle(WinHttpConnect(
            session_.get(),
            normalized_host.c_str(),
            port,
            0));
        if (!handle.get()) {
            if (error) {
                *error =
                    L"无法连接 OCR 依赖服务器：" +
                    windows_error_message(GetLastError());
            }
            return nullptr;
        }
        connections_.push_back(
            Connection{
                std::move(normalized_host),
                port,
                std::move(handle),
            });
        return connections_.back().handle.get();
    }

private:
    struct Connection {
        std::wstring host;
        INTERNET_PORT port{};
        UniqueWinHttpHandle handle;
    };

    UniqueWinHttpHandle session_;
    std::vector<Connection> connections_;
};

class ScopedDirectoryCleanup {
public:
    explicit ScopedDirectoryCleanup(std::filesystem::path path) : path_(std::move(path)) {}
    ~ScopedDirectoryCleanup() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    ScopedDirectoryCleanup(const ScopedDirectoryCleanup&) = delete;
    ScopedDirectoryCleanup& operator=(const ScopedDirectoryCleanup&) = delete;

    void release() noexcept {
        path_.clear();
    }

private:
    std::filesystem::path path_;
};

std::wstring normalized_hex(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character >= L'a' && character <= L'f') {
            result.push_back(static_cast<wchar_t>(character - L'a' + L'A'));
        } else if ((character >= L'A' && character <= L'F') ||
                   (character >= L'0' && character <= L'9')) {
            result.push_back(character);
        } else {
            return {};
        }
    }
    return result;
}

std::optional<std::vector<std::uint8_t>> decode_hex(std::wstring_view value) {
    const std::wstring normalized = normalized_hex(value);
    if (normalized.empty() || normalized.size() % 2 != 0) {
        return std::nullopt;
    }

    auto nibble = [](wchar_t character) -> std::uint8_t {
        return character <= L'9'
                   ? static_cast<std::uint8_t>(character - L'0')
                   : static_cast<std::uint8_t>(character - L'A' + 10);
    };

    std::vector<std::uint8_t> bytes(normalized.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(
            (nibble(normalized[i * 2]) << 4U) | nibble(normalized[i * 2 + 1]));
    }
    return bytes;
}

bool is_zero_hash(std::wstring_view value) {
    return value.size() == 64 &&
           std::ranges::all_of(value, [](wchar_t character) { return character == L'0'; });
}

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
    std::wstring path(32768, L'\0');
    const DWORD size =
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        return {};
    }
    path.resize(size);
    return std::filesystem::path(path).parent_path();
}

std::vector<std::filesystem::path> ocr_dependency_roots() {
    return {
        config_directory() / L"ocr" / kRapidOcrOnnxPackageId,
        executable_directory() / L"ocr" / kRapidOcrOnnxPackageId,
    };
}

bool is_regular_non_reparse_file(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool is_directory_without_reparse(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

std::optional<UniqueHandle> open_locked_path(
    const std::filesystem::path& path,
    bool directory,
    std::wstring* error) {
    const DWORD flags =
        FILE_FLAG_OPEN_REPARSE_POINT |
        (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN);
    const DWORD desired_access =
        directory
            ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE
            : GENERIC_READ | FILE_READ_ATTRIBUTES;
    UniqueHandle handle(CreateFileW(
        path.c_str(),
        desired_access,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr));
    if (!handle.get() || handle.get() == INVALID_HANDLE_VALUE) {
        if (error) {
            *error =
                L"无法锁定 OCR 依赖路径：" + path.wstring() + L"：" +
                windows_error_message(GetLastError());
        }
        return std::nullopt;
    }

    FILE_ATTRIBUTE_TAG_INFO information{};
    if (!GetFileInformationByHandleEx(
            handle.get(),
            FileAttributeTagInfo,
            &information,
            sizeof(information)) ||
        (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) !=
            directory ||
        GetFileType(handle.get()) != FILE_TYPE_DISK) {
        if (error) {
            *error = L"OCR 依赖路径类型无效或包含 reparse point：" +
                     path.wstring();
        }
        return std::nullopt;
    }
    FILE_STANDARD_INFO standard_information{};
    if (!GetFileInformationByHandleEx(
            handle.get(),
            FileStandardInfo,
            &standard_information,
            sizeof(standard_information)) ||
        standard_information.DeletePending) {
        if (error) {
            *error =
                L"OCR 依赖路径正被删除或无法读取元数据：" +
                path.wstring();
        }
        return std::nullopt;
    }
    return handle;
}

std::optional<std::uint64_t> file_size_from_handle(
    HANDLE handle,
    std::wstring* error) {
    FILE_STANDARD_INFO information{};
    if (!GetFileInformationByHandleEx(
            handle,
            FileStandardInfo,
            &information,
            sizeof(information)) ||
        information.EndOfFile.QuadPart < 0 ||
        information.DeletePending) {
        if (error) {
            *error =
                L"无法读取 OCR 依赖文件大小：" +
                windows_error_message(GetLastError());
        }
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(
        information.EndOfFile.QuadPart);
}

bool rewind_handle(HANDLE handle, std::wstring* error) {
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(
            handle,
            beginning,
            nullptr,
            FILE_BEGIN)) {
        if (error) {
            *error =
                L"无法定位 OCR 依赖文件：" +
                windows_error_message(GetLastError());
        }
        return false;
    }
    return true;
}

std::optional<std::vector<std::byte>> read_binary_handle(
    HANDLE handle,
    std::uint64_t maximum_size,
    std::wstring* error) {
    const auto size = file_size_from_handle(handle, error);
    if (!size || *size == 0 || *size > maximum_size ||
        *size > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()) ||
        !rewind_handle(handle, error)) {
        if (size && (*size == 0 || *size > maximum_size) && error) {
            *error = L"OCR 元数据文件大小超出允许范围。";
        }
        return std::nullopt;
    }

    std::vector<std::byte> bytes(
        static_cast<std::size_t>(*size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset,
            1024U * 1024U));
        DWORD bytes_read = 0;
        if (!ReadFile(
                handle,
                bytes.data() + offset,
                chunk,
                &bytes_read,
                nullptr) ||
            bytes_read != chunk) {
            if (error) {
                *error =
                    L"读取 OCR 元数据文件不完整：" +
                    windows_error_message(GetLastError());
            }
            return std::nullopt;
        }
        offset += bytes_read;
    }
    return bytes;
}

std::optional<std::array<std::uint8_t, 32>> sha256_handle(
    HANDLE handle,
    std::wstring* error,
    std::stop_token stop_token = {}) {
    if (ocr_stop_requested(stop_token, error)) {
        return std::nullopt;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> hash_object;
    std::array<std::uint8_t, 32> digest{};

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm,
        BCRYPT_SHA256_ALGORITHM,
        nullptr,
        0);
    DWORD object_size = 0;
    DWORD copied = 0;
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size),
            sizeof(object_size),
            &copied,
            0);
    }
    if (BCRYPT_SUCCESS(status)) {
        hash_object.resize(object_size);
        status = BCryptCreateHash(
            algorithm,
            &hash,
            hash_object.data(),
            static_cast<ULONG>(hash_object.size()),
            nullptr,
            0,
            0);
    }
    if (BCRYPT_SUCCESS(status) &&
        !rewind_handle(handle, error)) {
        status = static_cast<NTSTATUS>(0xC0000001L);
    }

    bool cancelled = false;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while (BCRYPT_SUCCESS(status)) {
        if (stop_token.stop_requested()) {
            cancelled = true;
            break;
        }
        DWORD bytes_read = 0;
        if (!ReadFile(
                handle,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytes_read,
                nullptr)) {
            status = static_cast<NTSTATUS>(0xC0000001L);
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        if (stop_token.stop_requested()) {
            cancelled = true;
            break;
        }
        status = BCryptHashData(
            hash,
            buffer.data(),
            bytes_read,
            0);
    }
    if (BCRYPT_SUCCESS(status) && !cancelled) {
        status = BCryptFinishHash(
            hash,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            0);
    }

    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    if (cancelled) {
        if (error) {
            *error = L"OCR 已取消。";
        }
        return std::nullopt;
    }
    if (!BCRYPT_SUCCESS(status)) {
        if (error && error->empty()) {
            *error = L"无法从锁定句柄计算 OCR 依赖 SHA256。";
        }
        return std::nullopt;
    }
    return digest;
}

std::wstring hex_digest(
    std::span<const std::uint8_t> bytes) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result;
    result.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

bool same_file_object(
    HANDLE first,
    HANDLE second) {
    FILE_ID_INFO first_id{};
    FILE_ID_INFO second_id{};
    return GetFileInformationByHandleEx(
               first,
               FileIdInfo,
               &first_id,
               sizeof(first_id)) &&
           GetFileInformationByHandleEx(
               second,
               FileIdInfo,
               &second_id,
               sizeof(second_id)) &&
           first_id.VolumeSerialNumber ==
               second_id.VolumeSerialNumber &&
           std::memcmp(
               first_id.FileId.Identifier,
               second_id.FileId.Identifier,
           sizeof(first_id.FileId.Identifier)) == 0;
}

std::optional<std::filesystem::path>
final_path_from_handle(
    HANDLE handle,
    std::wstring* error) {
    constexpr DWORD flags =
        FILE_NAME_NORMALIZED |
        VOLUME_NAME_DOS;
    const DWORD required =
        GetFinalPathNameByHandleW(
            handle,
            nullptr,
            0,
            flags);
    if (required == 0) {
        if (error) {
            *error =
                L"无法解析锁定 OCR 路径：" +
                windows_error_message(
                    GetLastError());
        }
        return std::nullopt;
    }

    std::wstring value(
        static_cast<std::size_t>(required) + 1,
        L'\0');
    const DWORD written =
        GetFinalPathNameByHandleW(
            handle,
            value.data(),
            static_cast<DWORD>(value.size()),
            flags);
    if (written == 0 ||
        written >= value.size()) {
        if (error) {
            *error =
                L"无法读取锁定 OCR 最终路径：" +
                windows_error_message(
                    GetLastError());
        }
        return std::nullopt;
    }
    value.resize(written);
    constexpr std::wstring_view unc_prefix =
        L"\\\\?\\UNC\\";
    constexpr std::wstring_view local_prefix =
        L"\\\\?\\";
    if (value.starts_with(unc_prefix)) {
        value =
            L"\\\\" +
            value.substr(unc_prefix.size());
    } else if (value.starts_with(local_prefix)) {
        value.erase(0, local_prefix.size());
    }
    return std::filesystem::path(value)
        .lexically_normal();
}

bool safe_existing_path_components(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    bool require_leaf) {
    if (!is_directory_without_reparse(root)) {
        return false;
    }

    std::filesystem::path current = root;
    std::size_t component_index = 0;
    const std::size_t component_count =
        static_cast<std::size_t>(std::distance(relative.begin(), relative.end()));
    for (const auto& component : relative) {
        current /= component;
        ++component_index;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return !require_leaf && component_index == component_count;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
        if (component_index < component_count &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return false;
        }
    }
    return true;
}

std::optional<LocalMemory> private_security_descriptor(
    std::wstring* error) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;OW)",
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
        if (error) {
            *error =
                L"无法创建 OCR 私有状态 ACL：" +
                windows_error_message(GetLastError());
        }
        return std::nullopt;
    }
    return LocalMemory(
        reinterpret_cast<HLOCAL>(descriptor));
}

bool handle_has_private_acl(
    HANDLE handle,
    std::wstring* error) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    const DWORD security_error = GetSecurityInfo(
        handle,
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION,
        &owner,
        nullptr,
        &dacl,
        nullptr,
        &raw_descriptor);
    LocalMemory descriptor(
        reinterpret_cast<HLOCAL>(raw_descriptor));
    if (security_error != ERROR_SUCCESS ||
        !owner || !dacl || !IsValidSid(owner)) {
        if (error) {
            *error =
                L"无法验证 OCR 序列状态 ACL：" +
                windows_error_message(security_error);
        }
        return false;
    }

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(
            raw_descriptor,
            &control,
            &revision) ||
        (control & SE_DACL_PROTECTED) == 0) {
        if (error) {
            *error = L"OCR 序列状态 ACL 未阻止权限继承。";
        }
        return false;
    }

    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_sid_buffer{};
    DWORD system_sid_size =
        static_cast<DWORD>(system_sid_buffer.size());
    if (!CreateWellKnownSid(
            WinLocalSystemSid,
            nullptr,
            system_sid_buffer.data(),
            &system_sid_size)) {
        if (error) {
            *error = L"无法创建 SYSTEM SID。";
        }
        return false;
    }

    std::array<std::byte, SECURITY_MAX_SID_SIZE>
        owner_rights_sid_buffer{};
    DWORD owner_rights_sid_size =
        static_cast<DWORD>(owner_rights_sid_buffer.size());
    if (!CreateWellKnownSid(
            WinCreatorOwnerRightsSid,
            nullptr,
            owner_rights_sid_buffer.data(),
            &owner_rights_sid_size)) {
        if (error) {
            *error = L"无法创建 Owner Rights SID。";
        }
        return false;
    }

    bool saw_system = false;
    bool saw_owner = false;
    for (DWORD index = 0;
         index < dacl->AceCount;
         ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(dacl, index, &raw_ace) || !raw_ace) {
            if (error) {
                *error = L"OCR 序列状态 ACL 结构无效。";
            }
            return false;
        }
        const auto* header =
            static_cast<const ACE_HEADER*>(raw_ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            if (error) {
                *error = L"OCR 序列状态 ACL 包含不允许的 ACE。";
            }
            return false;
        }
        const auto* ace =
            static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
        const PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if (!IsValidSid(sid) ||
            (ace->Mask & FILE_ALL_ACCESS) != FILE_ALL_ACCESS) {
            if (error) {
                *error = L"OCR 序列状态 ACL 未授予完整控制权。";
            }
            return false;
        }

        if (EqualSid(sid, system_sid_buffer.data())) {
            if (saw_system) {
                return false;
            }
            saw_system = true;
        } else if (
            EqualSid(sid, owner) ||
            EqualSid(
                sid,
                owner_rights_sid_buffer.data())) {
            if (saw_owner) {
                return false;
            }
            saw_owner = true;
        } else {
            if (error) {
                *error =
                    L"OCR 序列状态 ACL 向额外主体授予了访问权限。";
            }
            return false;
        }
    }
    if (!saw_system || !saw_owner ||
        dacl->AceCount != 2) {
        if (error) {
            *error =
                L"OCR 序列状态 ACL 必须仅允许所有者和 SYSTEM。";
        }
        return false;
    }
    return true;
}

std::optional<std::uint64_t> parse_sequence_value(
    std::string_view value) {
    if (value.empty() || value.size() > 16 ||
        (value.size() > 1 && value.front() == '0') ||
        !std::ranges::all_of(value, [](char character) {
            return character >= '0' && character <= '9';
        })) {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(
        value.data(),
        value.data() + value.size(),
        result);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() ||
        result < 1 || result > kMaxSafeJsonInteger) {
        return std::nullopt;
    }
    return result;
}

bool sequence_is_allowed_by_policy(
    std::uint64_t sequence,
    std::uint64_t persisted) noexcept {
    return sequence >=
               std::max(
                   kCompiledMinimumSequence,
                   persisted) &&
           sequence <= kMaxSafeJsonInteger;
}

std::optional<std::uint64_t> read_sequence_file_locked(
    const std::filesystem::path& path,
    bool allow_missing,
    std::wstring* error) {
    UniqueHandle handle(CreateFileW(
        path.c_str(),
        GENERIC_READ | READ_CONTROL,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle.get() ||
        handle.get() == INVALID_HANDLE_VALUE) {
        const DWORD open_error = GetLastError();
        if (allow_missing &&
            (open_error == ERROR_FILE_NOT_FOUND ||
             open_error == ERROR_PATH_NOT_FOUND)) {
            return 0;
        }
        if (error) {
            *error =
                L"无法读取 OCR 序列高水位：" +
                windows_error_message(open_error);
        }
        return std::nullopt;
    }

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileType(handle.get()) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(
            handle.get(),
            FileAttributeTagInfo,
            &attributes,
            sizeof(attributes)) ||
        (attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY |
          FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        !handle_has_private_acl(handle.get(), error)) {
        if (error && error->empty()) {
            *error =
                L"OCR 序列高水位文件类型或 ACL 不安全。";
        }
        return std::nullopt;
    }

    const auto bytes =
        read_binary_handle(handle.get(), 32, error);
    if (!bytes) {
        return std::nullopt;
    }
    const std::string_view text(
        reinterpret_cast<const char*>(bytes->data()),
        bytes->size());
    const auto sequence = parse_sequence_value(text);
    if (!sequence) {
        if (error) {
            *error =
                L"OCR 序列高水位不是严格的 ASCII 十进制整数。";
        }
        return std::nullopt;
    }
    return sequence;
}

std::filesystem::path sequence_high_watermark_path() {
    return config_directory() /
           std::wstring(kSequenceHighWatermarkName);
}

std::optional<std::uint64_t> read_sequence_high_watermark(
    std::wstring* error,
    std::stop_token stop_token = {}) {
    auto mutex = acquire_named_mutex(
        kSequenceMutexName,
        error,
        stop_token);
    if (!mutex) {
        return std::nullopt;
    }
    if (ocr_stop_requested(stop_token, error)) {
        return std::nullopt;
    }
    return read_sequence_file_locked(
        sequence_high_watermark_path(),
        true,
        error);
}

bool write_sequence_file_locked(
    const std::filesystem::path& path,
    std::uint64_t sequence,
    std::wstring* error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        path.parent_path(),
        filesystem_error);
    if (filesystem_error ||
        !is_directory_without_reparse(
            path.parent_path())) {
        if (error) {
            *error =
                L"无法创建安全的 OCR 序列状态目录。";
        }
        return false;
    }

    auto descriptor =
        private_security_descriptor(error);
    if (!descriptor) {
        return false;
    }
    SECURITY_ATTRIBUTES security{
        sizeof(security),
        descriptor->get(),
        FALSE,
    };

    std::filesystem::path temporary_path;
    UniqueHandle temporary_file;
    for (int attempt = 0; attempt < 16; ++attempt) {
        GUID guid{};
        wchar_t guid_text[40]{};
        if (FAILED(CoCreateGuid(&guid)) ||
            StringFromGUID2(
                guid,
                guid_text,
                static_cast<int>(
                    std::size(guid_text))) <= 0) {
            continue;
        }
        temporary_path =
            path.parent_path() /
            (L".ocr-sequence-" +
             std::wstring(guid_text) + L".tmp");
        temporary_file.reset(CreateFileW(
            temporary_path.c_str(),
            GENERIC_READ | GENERIC_WRITE |
                READ_CONTROL,
            0,
            &security,
            CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN |
                FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                FILE_FLAG_WRITE_THROUGH,
            nullptr));
        if (temporary_file.get() &&
            temporary_file.get() != INVALID_HANDLE_VALUE) {
            break;
        }
        temporary_file.reset();
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    if (!temporary_file.get()) {
        if (error) {
            *error =
                L"无法创建 OCR 序列高水位临时文件：" +
                windows_error_message(GetLastError());
        }
        return false;
    }

    const std::string text =
        std::to_string(sequence);
    DWORD written = 0;
    if (!WriteFile(
            temporary_file.get(),
            text.data(),
            static_cast<DWORD>(text.size()),
            &written,
            nullptr) ||
        written != text.size() ||
        !FlushFileBuffers(temporary_file.get()) ||
        !handle_has_private_acl(
            temporary_file.get(),
            error)) {
        temporary_file.reset();
        DeleteFileW(temporary_path.c_str());
        if (error && error->empty()) {
            *error =
                L"无法持久化 OCR 序列高水位。";
        }
        return false;
    }
    temporary_file.reset();

    if (!MoveFileExW(
            temporary_path.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = GetLastError();
        DeleteFileW(temporary_path.c_str());
        if (error) {
            *error =
                L"无法原子更新 OCR 序列高水位：" +
                windows_error_message(move_error);
        }
        return false;
    }

    const auto verified = read_sequence_file_locked(
        path,
        false,
        error);
    return verified && *verified == sequence;
}

bool persist_sequence_high_watermark(
    std::uint64_t sequence,
    std::wstring* error,
    std::stop_token stop_token = {}) {
    if (ocr_stop_requested(stop_token, error)) {
        return false;
    }
    if (sequence < 1 ||
        sequence > kMaxSafeJsonInteger) {
        if (error) {
            *error = L"OCR 序列号超出有效范围。";
        }
        return false;
    }

    auto mutex = acquire_named_mutex(
        kSequenceMutexName,
        error,
        stop_token);
    if (!mutex) {
        return false;
    }
    if (ocr_stop_requested(stop_token, error)) {
        return false;
    }
    const auto existing = read_sequence_file_locked(
        sequence_high_watermark_path(),
        true,
        error);
    if (!existing) {
        return false;
    }
    if (sequence < *existing) {
        if (error) {
            *error = std::format(
                L"已拒绝 OCR 依赖回滚：持久高水位为 {}，候选序列为 {}。",
                *existing,
                sequence);
        }
        return false;
    }
    if (sequence == *existing) {
        return true;
    }
    return write_sequence_file_locked(
        sequence_high_watermark_path(),
        sequence,
        error);
}

std::wstring profile_relative_path(const OcrEngineSpec& spec, std::wstring_view file) {
    std::wstring result(spec.profile_directory);
    result += L"/";
    result += file;
    return result;
}

std::vector<std::wstring> required_dependency_files(const OcrEngineSpec& spec) {
    std::vector<std::wstring> files;
    files.reserve(kRapidOcrCommonRequiredFiles.size() + kRapidOcrProfileRequiredFiles.size());
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

bool reserved_windows_component(std::wstring_view component) {
    std::wstring base(component.substr(0, component.find(L'.')));
    std::ranges::transform(base, base.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towupper(character));
    });
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") {
        return true;
    }
    if (base.size() == 4 &&
        (base.starts_with(L"COM") || base.starts_with(L"LPT")) &&
        base[3] >= L'1' && base[3] <= L'9') {
        return true;
    }
    return false;
}

std::optional<std::wstring> normalize_manifest_relative_path(std::wstring_view value) {
    if (value.empty() || value.size() > 240 ||
        value.starts_with(L"\\") || value.starts_with(L"/") ||
        value.ends_with(L"\\") || value.ends_with(L"/")) {
        return std::nullopt;
    }

    std::wstring normalized(value);
    std::ranges::replace(normalized, L'\\', L'/');
    const std::wstring_view normalized_view(normalized);
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const std::size_t slash = normalized.find(L'/', start);
        const std::wstring_view component = normalized_view.substr(
            start,
            slash == std::wstring::npos ? normalized.size() - start : slash - start);
        if (component.empty() || component == L"." || component == L".." ||
            component.ends_with(L".") || component.ends_with(L" ") ||
            reserved_windows_component(component)) {
            return std::nullopt;
        }
        for (const wchar_t character : component) {
            const bool ascii_letter =
                (character >= L'a' && character <= L'z') ||
                (character >= L'A' && character <= L'Z');
            const bool ascii_digit =
                character >= L'0' && character <= L'9';
            if (!ascii_letter && !ascii_digit && character != L'.' &&
                character != L'_' && character != L'-') {
                return std::nullopt;
            }
        }
        if (slash == std::wstring::npos) {
            break;
        }
        start = slash + 1;
    }
    return normalized;
}

std::wstring path_comparison_key(std::wstring_view path) {
    std::wstring result(path);
    std::ranges::transform(result, result.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

bool valid_https_url(std::wstring_view value) {
    if (value.size() < 10 || value.size() > 2'048) {
        return false;
    }
    constexpr std::wstring_view prefix = L"https://";
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::towlower(value[i]) != prefix[i]) {
            return false;
        }
    }
    const std::size_t host_end = value.find_first_of(L"/?#", prefix.size());
    const std::wstring_view host = value.substr(
        prefix.size(),
        host_end == std::wstring_view::npos ? value.size() - prefix.size()
                                            : host_end - prefix.size());
    return !host.empty() && host.find(L'@') == std::wstring_view::npos;
}

bool manifest_contains_required_files(const OcrDependencyManifest& manifest) {
    for (const auto& required : all_required_dependency_files()) {
        const auto found = std::ranges::find_if(
            manifest.files,
            [&required](const OcrDependencyFile& file) {
                return path_comparison_key(file.path) == path_comparison_key(required);
            });
        if (found == manifest.files.end()) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<std::byte>> read_binary_file(
    const std::filesystem::path& path,
    std::uint64_t maximum_size,
    std::wstring* error = nullptr) {
    try {
        if (!is_regular_non_reparse_file(path)) {
            if (error) {
                *error = L"文件不存在或是不安全的 reparse point。";
            }
            return std::nullopt;
        }
        std::error_code size_error;
        const std::uint64_t size = std::filesystem::file_size(path, size_error);
        if (size_error || size == 0 || size > maximum_size ||
            size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
            if (error) {
                *error = L"文件大小超出允许范围。";
            }
            return std::nullopt;
        }

        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            if (error) {
                *error = L"无法读取文件。";
            }
            return std::nullopt;
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        stream.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
            if (error) {
                *error = L"文件读取不完整。";
            }
            return std::nullopt;
        }
        return bytes;
    } catch (const std::exception& exception) {
        if (error) {
            *error = from_utf8(exception.what());
        }
        return std::nullopt;
    }
}

bool write_binary_file(
    const std::filesystem::path& path,
    std::span<const std::byte> bytes,
    std::wstring* error) {
    try {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            if (error) {
                *error = L"无法创建 OCR 清单元数据。";
            }
            return false;
        }
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            if (error) {
                *error = L"无法完整写入 OCR 清单元数据。";
            }
            return false;
        }
        return true;
    } catch (...) {
        if (error) {
            *error = L"写入 OCR 清单元数据时发生异常。";
        }
        return false;
    }
}

bool download_file_attempt(
    WinHttpDownloadContext& context,
    std::wstring_view url,
    const std::filesystem::path& path,
    std::uint64_t maximum_size,
    ULONGLONG operation_deadline,
    std::stop_token stop_token,
    bool* retryable_timeout,
    std::wstring* error) {
    if (retryable_timeout) {
        *retryable_timeout = false;
    }
    if (!valid_https_url(url)) {
        if (error) {
            *error = L"下载地址必须是有效的 HTTPS URL。";
        }
        return false;
    }
    if (stop_token.stop_requested()) {
        if (error) {
            *error = L"OCR 依赖下载已取消。";
        }
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error || !is_directory_without_reparse(path.parent_path())) {
        if (error) {
            *error = L"无法创建安全的 OCR 下载目录。";
        }
        return false;
    }
    std::filesystem::remove(path, filesystem_error);

    std::wstring url_copy(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(
            url_copy.c_str(),
            static_cast<DWORD>(url_copy.size()),
            0,
            &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.dwHostNameLength == 0) {
        if (error) {
            *error = L"无法解析 OCR 依赖 HTTPS 地址。";
        }
        return false;
    }

    const std::wstring host(
        components.lpszHostName,
        components.dwHostNameLength);
    std::wstring request_path(
        components.lpszUrlPath,
        components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        request_path.append(
            components.lpszExtraInfo,
            components.dwExtraInfoLength);
    }
    if (request_path.empty()) {
        request_path = L"/";
    }

    const HINTERNET connection =
        context.connection(host, components.nPort, error);
    if (!connection) {
        return false;
    }

    UniqueWinHttpHandle request(WinHttpOpenRequest(
        connection,
        L"GET",
        request_path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request.get()) {
        if (error) {
            *error =
                L"无法创建 OCR 下载请求：" +
                windows_error_message(GetLastError());
        }
        return false;
    }
    if (stop_token.stop_requested()) {
        if (error) {
            *error = L"OCR 依赖下载已取消。";
        }
        return false;
    }

    DWORD redirect_policy =
        WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    DWORD maximum_redirects = 5;
    if (!WinHttpSetOption(
            request.get(),
            WINHTTP_OPTION_REDIRECT_POLICY,
            &redirect_policy,
            sizeof(redirect_policy)) ||
        !WinHttpSetOption(
            request.get(),
            WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
            &maximum_redirects,
            sizeof(maximum_redirects)) ||
        !WinHttpAddRequestHeaders(
            request.get(),
            L"Accept-Encoding: identity\r\n",
            static_cast<DWORD>(-1),
            WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD)) {
        if (error) {
            *error =
                L"无法配置 OCR 下载安全策略：" +
                windows_error_message(GetLastError());
        }
        return false;
    }

    const ULONGLONG request_deadline = std::min(
        operation_deadline,
        GetTickCount64() + kDownloadFileDeadlineMs);
    if (GetTickCount64() >= request_deadline ||
        stop_token.stop_requested()) {
        if (error) {
            *error = stop_token.stop_requested()
                         ? L"OCR 依赖下载已取消。"
                         : L"OCR 依赖下载超过总时间预算。";
        }
        return false;
    }
    DWORD request_error = ERROR_SUCCESS;
    if (!WinHttpSendRequest(
            request.get(),
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0)) {
        request_error = GetLastError();
        if (retryable_timeout) {
            *retryable_timeout =
                request_error ==
                    ERROR_WINHTTP_TIMEOUT ||
                request_error ==
                    ERROR_WINHTTP_RESEND_REQUEST;
        }
        if (error) {
            *error =
                stop_token.stop_requested()
                    ? L"OCR 依赖下载已取消。"
                    : request_error ==
                              ERROR_WINHTTP_TIMEOUT
                          ? L"OCR 下载请求超过时间预算。"
                          : L"OCR 下载请求失败：" +
                                windows_error_message(request_error);
        }
        return false;
    }
    if (stop_token.stop_requested()) {
        if (error) {
            *error = L"OCR 依赖下载已取消。";
        }
        return false;
    }
    if (!WinHttpReceiveResponse(
            request.get(),
            nullptr)) {
        request_error = GetLastError();
        if (retryable_timeout) {
            *retryable_timeout =
                request_error ==
                    ERROR_WINHTTP_TIMEOUT ||
                request_error ==
                    ERROR_WINHTTP_RESEND_REQUEST;
        }
        if (error) {
            *error =
                stop_token.stop_requested()
                    ? L"OCR 依赖下载已取消。"
                    : L"OCR 下载请求失败：" +
                          windows_error_message(
                              request_error);
        }
        return false;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX) ||
        status_code != HTTP_STATUS_OK) {
        if (error) {
            *error = std::format(
                L"OCR 下载服务器返回 HTTP {}。",
                status_code);
        }
        return false;
    }

    DWORD final_url_bytes = 0;
    WinHttpQueryOption(
        request.get(),
        WINHTTP_OPTION_URL,
        nullptr,
        &final_url_bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        final_url_bytes < sizeof(wchar_t)) {
        if (error) {
            *error = L"无法验证 OCR 下载重定向目标。";
        }
        return false;
    }
    std::vector<wchar_t> final_url(
        final_url_bytes / sizeof(wchar_t));
    if (!WinHttpQueryOption(
            request.get(),
            WINHTTP_OPTION_URL,
            final_url.data(),
            &final_url_bytes) ||
        !valid_https_url(final_url.data())) {
        if (error) {
            *error = L"OCR 下载重定向目标不是可信的 HTTPS URL。";
        }
        return false;
    }

    DWORD content_length = 0;
    DWORD content_length_size = sizeof(content_length);
    if (WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &content_length,
            &content_length_size,
            WINHTTP_NO_HEADER_INDEX) &&
        (content_length == 0 || content_length > maximum_size)) {
        if (error) {
            *error = L"OCR 下载响应大小超出允许范围。";
        }
        return false;
    }

    SECURITY_ATTRIBUTES file_security{
        sizeof(file_security),
        nullptr,
        FALSE,
    };
    UniqueHandle output_file(CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        &file_security,
        CREATE_NEW,
        FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_TEMPORARY |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!output_file.get() || output_file.get() == INVALID_HANDLE_VALUE) {
        if (error) {
            *error =
                L"无法创建 OCR 下载文件：" +
                windows_error_message(GetLastError());
        }
        return false;
    }

    auto fail_and_remove = [&](std::wstring message) {
        output_file.reset();
        std::filesystem::remove(path, filesystem_error);
        if (error) {
            *error = std::move(message);
        }
        return false;
    };

    std::array<std::uint8_t, 64U * 1024U> buffer{};
    std::uint64_t total_bytes = 0;
    for (;;) {
        if (stop_token.stop_requested()) {
            return fail_and_remove(L"OCR 依赖下载已取消。");
        }
        if (GetTickCount64() >= request_deadline) {
            return fail_and_remove(L"OCR 依赖下载超过时间预算。");
        }

        DWORD bytes_read = 0;
        if (!WinHttpReadData(
                request.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytes_read)) {
            const DWORD read_error =
                GetLastError();
            if (retryable_timeout) {
                *retryable_timeout =
                    read_error ==
                    ERROR_WINHTTP_TIMEOUT;
            }
            return fail_and_remove(
                stop_token.stop_requested()
                    ? L"OCR 依赖下载已取消。"
                    : read_error ==
                              ERROR_WINHTTP_TIMEOUT
                          ? L"读取 OCR 下载响应超过时间预算。"
                          : L"读取 OCR 下载响应失败：" +
                                windows_error_message(read_error));
        }
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read > maximum_size ||
            total_bytes > maximum_size - bytes_read) {
            return fail_and_remove(L"OCR 下载响应大小超出允许范围。");
        }

        DWORD bytes_written = 0;
        if (!WriteFile(
                output_file.get(),
                buffer.data(),
                bytes_read,
                &bytes_written,
                nullptr) ||
            bytes_written != bytes_read) {
            return fail_and_remove(
                L"写入 OCR 下载文件失败：" +
                windows_error_message(GetLastError()));
        }
        total_bytes += bytes_read;
    }

    if (total_bytes == 0 || !FlushFileBuffers(output_file.get())) {
        return fail_and_remove(
            total_bytes == 0
                ? L"OCR 下载响应为空。"
                : L"刷新 OCR 下载文件失败：" +
                      windows_error_message(GetLastError()));
    }
    output_file.reset();
    if (!is_regular_non_reparse_file(path)) {
        std::filesystem::remove(path, filesystem_error);
        if (error) {
            *error = L"下载目标不是常规文件。";
        }
        return false;
    }
    return true;
}

bool download_file(
    WinHttpDownloadContext& context,
    std::wstring_view url,
    const std::filesystem::path& path,
    std::uint64_t maximum_size,
    ULONGLONG operation_deadline,
    std::stop_token stop_token,
    std::wstring* error) {
    const ULONGLONG file_deadline =
        std::min(
            operation_deadline,
            GetTickCount64() +
                kDownloadFileDeadlineMs);
    for (int attempt = 0;
         attempt < kMaxDownloadAttempts;
         ++attempt) {
        if (stop_token.stop_requested()) {
            if (error) {
                *error =
                    L"OCR 依赖下载已取消。";
            }
            return false;
        }
        if (GetTickCount64() >= file_deadline) {
            if (error) {
                *error =
                    L"OCR 依赖下载超过总时间预算。";
            }
            return false;
        }

        bool retryable_timeout = false;
        std::wstring attempt_error;
        if (download_file_attempt(
                context,
                url,
                path,
                maximum_size,
                file_deadline,
                stop_token,
                &retryable_timeout,
                &attempt_error)) {
            return true;
        }
        if (!retryable_timeout ||
            stop_token.stop_requested() ||
            GetTickCount64() >= file_deadline) {
            if (error) {
                *error =
                    std::move(attempt_error);
            }
            return false;
        }
    }
    if (error) {
        *error =
            L"OCR 下载在连续超时后停止重试。";
    }
    return false;
}

bool verify_dependency_file(
    const OcrDependencyFile& file,
    const std::filesystem::path& path,
    std::wstring* error,
    bool verify_hash = true) {
    if (!is_regular_non_reparse_file(path)) {
        if (error) {
            *error = L"OCR 依赖文件不存在或是不安全的 reparse point。";
        }
        return false;
    }

    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || file.size == 0 || size != file.size) {
        if (error) {
            *error = L"OCR 依赖文件大小与清单不一致。";
        }
        return false;
    }

    if (verify_hash) {
        std::wstring hash_error;
        const std::wstring actual_hash = sha256_file(path, &hash_error);
        if (actual_hash != normalized_hex(file.sha256)) {
            if (error) {
                *error =
                    hash_error.empty() ? L"OCR 依赖文件 SHA256 校验失败。"
                                       : hash_error;
            }
            return false;
        }
    }
    return true;
}

std::wstring configured_manifest_key_id() {
    return from_utf8(AIRSHOT_OCR_MANIFEST_KEY_ID);
}

std::optional<std::array<std::uint8_t, 64>> configured_manifest_public_key() {
    const std::wstring hex = from_utf8(AIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX);
    const auto bytes = decode_hex(hex);
    if (!bytes || bytes->size() != 64) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 64> result{};
    std::ranges::copy(*bytes, result.begin());
    return result;
}

bool validate_signed_manifest(
    std::span<const std::byte> manifest_bytes,
    std::span<const std::byte> signature_bytes,
    bool require_current,
    OcrDependencyManifest* parsed_manifest,
    std::wstring* error) {
    const std::wstring key_id = configured_manifest_key_id();
    const auto public_key = configured_manifest_public_key();
    if (key_id.empty() || !public_key) {
        if (error) {
            *error =
                L"此构建未配置 OCR 清单公钥，无法验证依赖。";
        }
        return false;
    }

    const std::string_view signature_utf8(
        reinterpret_cast<const char*>(signature_bytes.data()),
        signature_bytes.size());
    const std::wstring signature_json = from_utf8(signature_utf8);
    if (signature_json.empty()) {
        if (error) {
            *error = L"OCR 清单签名文件不是有效的 UTF-8 JSON。";
        }
        return false;
    }
    const auto signature = parse_ocr_manifest_signature(signature_json);
    if (!signature || signature->key_id != key_id) {
        if (error) {
            *error = L"OCR 清单签名 keyId 无效或不受信任。";
        }
        return false;
    }
    if (!verify_ocr_manifest_signature(
            manifest_bytes,
            signature->value,
            *public_key,
            error)) {
        return false;
    }

    const std::string_view manifest_utf8(
        reinterpret_cast<const char*>(manifest_bytes.data()),
        manifest_bytes.size());
    const std::wstring manifest_json = from_utf8(manifest_utf8);
    if (manifest_json.empty()) {
        if (error) {
            *error = L"OCR 依赖清单不是有效的 UTF-8 JSON。";
        }
        return false;
    }
    const auto manifest = parse_ocr_dependency_manifest(manifest_json);
    if (!manifest) {
        if (error) {
            *error = L"OCR 依赖清单格式无效或缺少必要文件。";
        }
        return false;
    }
    if (require_current) {
        const auto now_value = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_value < 0) {
            if (error) {
                *error = L"系统时间无效，无法验证 OCR 清单有效期。";
            }
            return false;
        }
        const std::uint64_t now = static_cast<std::uint64_t>(now_value);
        if (manifest->issued_at > now + kManifestClockSkewSeconds) {
            if (error) {
                *error = L"OCR 依赖清单签发时间晚于当前系统时间。";
            }
            return false;
        }
        if (now > manifest->expires_at + kManifestClockSkewSeconds) {
            if (error) {
                *error = L"OCR 依赖清单已过期，请获取最新版本。";
            }
            return false;
        }
    }
    if (parsed_manifest) {
        *parsed_manifest = *manifest;
    }
    return true;
}

bool dependency_hash_cache_key_matches_impl(
    bool same_file_object_value,
    const std::filesystem::path& cached_root,
    std::uint64_t cached_sequence,
    std::wstring_view cached_relative_path,
    std::wstring_view cached_sha256,
    std::uint64_t cached_size,
    const std::filesystem::path& requested_root,
    std::uint64_t requested_sequence,
    std::wstring_view requested_relative_path,
    std::wstring_view requested_sha256,
    std::uint64_t requested_size) {
    return same_file_object_value &&
           cached_sequence == requested_sequence &&
           cached_size == requested_size &&
           path_comparison_key(
               cached_root.lexically_normal().generic_wstring()) ==
               path_comparison_key(
                   requested_root.lexically_normal().generic_wstring()) &&
           path_comparison_key(cached_relative_path) ==
               path_comparison_key(requested_relative_path) &&
           normalized_hex(cached_sha256) ==
               normalized_hex(requested_sha256);
}

struct CachedDependencyHashEntry {
    std::wstring relative_path;
    std::wstring sha256;
    std::uint64_t size{};
    UniqueHandle guard;
};

struct DependencyHashCacheState {
    std::mutex mutex;
    std::uint64_t generation{};
    std::filesystem::path root;
    std::uint64_t sequence{};
    std::map<std::wstring, CachedDependencyHashEntry> files;
};

DependencyHashCacheState& dependency_hash_cache() {
    static DependencyHashCacheState cache;
    return cache;
}

std::uint64_t dependency_hash_cache_generation() {
    auto& cache = dependency_hash_cache();
    std::scoped_lock lock(cache.mutex);
    return cache.generation;
}

bool dependency_hash_cache_matches(
    const std::filesystem::path& root,
    std::uint64_t sequence,
    const OcrDependencyFile& file,
    HANDLE current_handle) {
    auto& cache = dependency_hash_cache();
    std::scoped_lock lock(cache.mutex);
    const auto found = cache.files.find(
        path_comparison_key(file.path));
    if (found == cache.files.end()) {
        return false;
    }
    const CachedDependencyHashEntry& cached =
        found->second;
    return dependency_hash_cache_key_matches_impl(
        same_file_object(
            cached.guard.get(),
            current_handle),
        cache.root,
        cache.sequence,
        cached.relative_path,
        cached.sha256,
        cached.size,
        root,
        sequence,
        file.path,
        file.sha256,
        file.size);
}

std::optional<CachedDependencyHashEntry>
make_cached_dependency_hash_entry(
    const OcrDependencyFile& file,
    HANDLE current_handle) {
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(
            GetCurrentProcess(),
            current_handle,
            GetCurrentProcess(),
            &duplicate,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS)) {
        return std::nullopt;
    }
    return CachedDependencyHashEntry{
        file.path,
        normalized_hex(file.sha256),
        file.size,
        UniqueHandle(duplicate),
    };
}

void publish_dependency_hash_cache(
    std::uint64_t expected_generation,
    const std::filesystem::path& root,
    std::uint64_t sequence,
    std::map<std::wstring, CachedDependencyHashEntry> files) {
    auto& cache = dependency_hash_cache();
    std::scoped_lock lock(cache.mutex);
    if (cache.generation != expected_generation) {
        return;
    }
    cache.root = root;
    cache.sequence = sequence;
    cache.files = std::move(files);
    ++cache.generation;
}

void clear_dependency_hash_cache() {
    auto& cache = dependency_hash_cache();
    std::scoped_lock lock(cache.mutex);
    cache.root.clear();
    cache.sequence = 0;
    cache.files.clear();
    ++cache.generation;
}

bool verify_dependency_handle(
    const OcrDependencyFile& file,
    HANDLE handle,
    bool verify_hash,
    std::stop_token stop_token,
    std::wstring* error) {
    if (ocr_stop_requested(stop_token, error)) {
        return false;
    }
    const auto size =
        file_size_from_handle(handle, error);
    if (!size || file.size == 0 ||
        *size != file.size) {
        if (error && error->empty()) {
            *error =
                L"OCR 依赖文件大小与清单不一致。";
        }
        return false;
    }
    if (!verify_hash) {
        return true;
    }

    const auto digest =
        sha256_handle(handle, error, stop_token);
    if (!digest ||
        hex_digest(*digest) !=
            normalized_hex(file.sha256)) {
        if (error && error->empty()) {
            *error =
                L"OCR 依赖文件 SHA256 校验失败。";
        }
        return false;
    }
    return true;
}

struct VerifiedDependency {
    std::vector<UniqueHandle> handles;
    OcrDependencyManifest manifest;
    std::filesystem::path root;
};

std::optional<VerifiedDependency>
verify_and_lock_installed_dependency(
    const std::filesystem::path& root,
    bool verify_hashes,
    std::uint64_t minimum_sequence,
    std::stop_token stop_token,
    std::wstring* error) {
    if (ocr_stop_requested(stop_token, error)) {
        return std::nullopt;
    }
    VerifiedDependency result;
    result.handles.reserve(
        kMaxManifestFiles +
        kMaxDependencyDirectories + 8);
    const std::uint64_t cache_generation =
        verify_hashes
            ? dependency_hash_cache_generation()
            : 0;
    std::map<std::wstring, CachedDependencyHashEntry>
        next_cached_files;
    bool cache_complete = verify_hashes;

    std::vector<std::filesystem::path>
        protected_directories;
    std::filesystem::path current = root;
    for (int depth = 0; depth < 3; ++depth) {
        if (ocr_stop_requested(stop_token, error)) {
            return std::nullopt;
        }
        if (current.empty() ||
            current == current.root_path()) {
            break;
        }
        protected_directories.push_back(current);
        current = current.parent_path();
    }
    std::ranges::reverse(protected_directories);

    HANDLE locked_root = nullptr;
    for (const auto& directory :
         protected_directories) {
        if (ocr_stop_requested(stop_token, error)) {
            return std::nullopt;
        }
        auto directory_handle =
            open_locked_path(
                directory,
                true,
                error);
        if (!directory_handle) {
            return std::nullopt;
        }
        if (directory == root) {
            locked_root =
                directory_handle->get();
        }
        result.handles.push_back(
            std::move(*directory_handle));
    }
    if (!locked_root) {
        if (error) {
            *error =
                L"无法锁定 OCR 依赖根目录。";
        }
        return std::nullopt;
    }
    const auto locked_root_path =
        final_path_from_handle(
            locked_root,
            error);
    if (!locked_root_path) {
        return std::nullopt;
    }
    result.root = *locked_root_path;

    auto manifest_handle = open_locked_path(
        result.root / kInstalledManifestName,
        false,
        error);
    auto signature_handle = open_locked_path(
        result.root / kInstalledSignatureName,
        false,
        error);
    if (!manifest_handle || !signature_handle) {
        if (error && error->empty()) {
            *error =
                L"OCR 依赖签名元数据缺失或不安全。";
        }
        return std::nullopt;
    }

    const auto manifest_bytes =
        read_binary_handle(
            manifest_handle->get(),
            kMaxManifestBytes,
            error);
    const auto signature_bytes =
        read_binary_handle(
            signature_handle->get(),
            64U * 1024U,
            error);
    if (!manifest_bytes || !signature_bytes ||
        !validate_signed_manifest(
            *manifest_bytes,
            *signature_bytes,
            false,
            &result.manifest,
            error)) {
        return std::nullopt;
    }
    if (ocr_stop_requested(stop_token, error)) {
        return std::nullopt;
    }
    if (!sequence_is_allowed_by_policy(
            result.manifest.sequence,
            minimum_sequence)) {
        if (error) {
            *error = std::format(
                L"已拒绝 OCR 依赖回滚：最低允许序列为 {}，已安装序列为 {}。",
                minimum_sequence,
                result.manifest.sequence);
        }
        return std::nullopt;
    }

    result.handles.push_back(
        std::move(*manifest_handle));
    result.handles.push_back(
        std::move(*signature_handle));

    std::map<std::wstring, std::filesystem::path>
        expected_directories;
    std::set<std::wstring> expected_files{
        path_comparison_key(
            kInstalledManifestName),
        path_comparison_key(
            kInstalledSignatureName),
    };
    for (const auto& entry :
         result.manifest.files) {
        if (ocr_stop_requested(stop_token, error)) {
            return std::nullopt;
        }
        const std::filesystem::path relative(
            entry.path);
        std::filesystem::path parent;
        for (const auto& component :
             relative.parent_path()) {
            parent /= component;
            expected_directories.emplace(
                path_comparison_key(
                    parent.generic_wstring()),
                parent);
        }
        expected_files.insert(
            path_comparison_key(entry.path));
    }
    if (expected_directories.size() >
        kMaxDependencyDirectories) {
        if (error) {
            *error =
                L"OCR 依赖清单包含过多目录。";
        }
        return std::nullopt;
    }

    std::vector<std::filesystem::path>
        ordered_directories;
    ordered_directories.reserve(
        expected_directories.size());
    for (const auto& [key, path] :
         expected_directories) {
        static_cast<void>(key);
        ordered_directories.push_back(path);
    }
    std::ranges::sort(
        ordered_directories,
        [](const std::filesystem::path& first,
           const std::filesystem::path& second) {
            const auto first_depth =
                std::distance(
                    first.begin(),
                    first.end());
            const auto second_depth =
                std::distance(
                    second.begin(),
                    second.end());
            if (first_depth != second_depth) {
                return first_depth < second_depth;
            }
            return first.generic_wstring() <
                   second.generic_wstring();
        });

    for (const auto& relative :
         ordered_directories) {
        if (ocr_stop_requested(stop_token, error)) {
            return std::nullopt;
        }
        auto directory_handle = open_locked_path(
            result.root / relative,
            true,
            error);
        if (!directory_handle) {
            return std::nullopt;
        }
        result.handles.push_back(
            std::move(*directory_handle));
    }

    for (const auto& entry :
         result.manifest.files) {
        if (ocr_stop_requested(stop_token, error)) {
            return std::nullopt;
        }
        auto file_handle = open_locked_path(
            result.root /
                std::filesystem::path(entry.path),
            false,
            error);
        const bool cached_hash =
            file_handle && verify_hashes &&
            dependency_hash_cache_matches(
                result.root,
                result.manifest.sequence,
                entry,
                file_handle->get());
        if (!file_handle ||
            !verify_dependency_handle(
                entry,
                file_handle->get(),
                verify_hashes && !cached_hash,
                stop_token,
                error)) {
            return std::nullopt;
        }
        if (cache_complete) {
            auto cached_entry =
                make_cached_dependency_hash_entry(
                    entry,
                    file_handle->get());
            if (!cached_entry) {
                cache_complete = false;
                next_cached_files.clear();
            } else {
                const auto [position, inserted] =
                    next_cached_files.emplace(
                        path_comparison_key(entry.path),
                        std::move(*cached_entry));
                static_cast<void>(position);
                if (!inserted) {
                    cache_complete = false;
                    next_cached_files.clear();
                }
            }
        }
        result.handles.push_back(
            std::move(*file_handle));
    }

    std::error_code iterator_error;
    std::filesystem::recursive_directory_iterator
        iterator(
            result.root,
            std::filesystem::directory_options::none,
            iterator_error);
    const std::filesystem::recursive_directory_iterator
        end;
    for (;
         !iterator_error && iterator != end;
         iterator.increment(iterator_error)) {
        if (ocr_stop_requested(stop_token, error)) {
            return std::nullopt;
        }
        const std::filesystem::path relative_path =
            iterator->path().lexically_relative(
                result.root);
        const std::wstring relative =
            relative_path.generic_wstring();
        const std::wstring key =
            path_comparison_key(relative);

        const DWORD attributes =
            GetFileAttributesW(
                iterator->path().c_str());
        if (attributes ==
                INVALID_FILE_ATTRIBUTES ||
            (attributes &
             FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (error) {
                *error =
                    L"OCR 依赖目录包含不安全的 reparse point：" +
                    relative;
            }
            return std::nullopt;
        }
        if ((attributes &
             FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!expected_directories.contains(key)) {
                if (error) {
                    *error =
                        L"OCR 依赖目录包含清单外目录：" +
                        relative;
                }
                return std::nullopt;
            }
        } else if (!expected_files.contains(key)) {
            if (error) {
                *error =
                    L"OCR 依赖目录包含清单外文件：" +
                    relative;
            }
            return std::nullopt;
        }
    }
    if (iterator_error) {
        if (error) {
            *error =
                L"无法完整枚举 OCR 依赖目录。";
        }
        return std::nullopt;
    }

    if (ocr_stop_requested(stop_token, error)) {
        return std::nullopt;
    }

    auto final_root_handle =
        open_locked_path(
            result.root,
            true,
            error);
    if (!final_root_handle ||
        !same_file_object(
            locked_root,
            final_root_handle->get())) {
        if (error && error->empty()) {
            *error =
                L"OCR 依赖根目录在校验期间发生变化。";
        }
        return std::nullopt;
    }
    result.handles.push_back(
        std::move(*final_root_handle));
    if (cache_complete) {
        publish_dependency_hash_cache(
            cache_generation,
            result.root,
            result.manifest.sequence,
            std::move(next_cached_files));
    }
    return result;
}

bool verify_installed_manifest(
    const std::filesystem::path& root,
    bool verify_hashes,
    std::wstring* error) {
    return acquire_ocr_dependency_lease(
               root,
               verify_hashes,
               false,
               error)
        .has_value();
}

bool dependency_directory_ready(
    const std::filesystem::path& root,
    const OcrEngineSpec& spec,
    std::wstring* missing_file = nullptr,
    bool verify_hashes = false) {
    if (!is_directory_without_reparse(root)) {
        return false;
    }
    for (const auto& file : required_dependency_files(spec)) {
        const std::filesystem::path relative(file);
        if (!safe_existing_path_components(root, relative, true) ||
            !is_regular_non_reparse_file(root / relative)) {
            if (missing_file) {
                *missing_file = file;
            }
            return false;
        }
    }

    std::wstring verification_error;
    if (!verify_installed_manifest(root, verify_hashes, &verification_error)) {
        if (missing_file) {
            *missing_file =
                verification_error.empty() ? L"签名或哈希校验失败" : verification_error;
        }
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> create_private_directory(
    const std::filesystem::path& parent,
    std::wstring_view prefix,
    std::wstring* error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(parent, filesystem_error);
    if (filesystem_error || !is_directory_without_reparse(parent)) {
        if (error) {
            *error = L"无法创建安全的 OCR 临时目录根。";
        }
        return std::nullopt;
    }

    using ConvertSddl = BOOL(WINAPI*)(
        LPCWSTR,
        DWORD,
        PSECURITY_DESCRIPTOR*,
        PULONG);
    HMODULE advapi = LoadLibraryExW(
        L"advapi32.dll",
        nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!advapi) {
        if (error) {
            *error = L"无法加载 Windows ACL 支持。";
        }
        return std::nullopt;
    }
    const auto convert = reinterpret_cast<ConvertSddl>(
        GetProcAddress(advapi, "ConvertStringSecurityDescriptorToSecurityDescriptorW"));
    if (!convert) {
        FreeLibrary(advapi);
        if (error) {
            *error = L"无法初始化 Windows ACL 支持。";
        }
        return std::nullopt;
    }

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!convert(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)",
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
        FreeLibrary(advapi);
        if (error) {
            *error = L"无法创建 OCR 临时目录 ACL。";
        }
        return std::nullopt;
    }

    SECURITY_ATTRIBUTES security{
        sizeof(security),
        descriptor,
        FALSE,
    };
    std::optional<std::filesystem::path> result;
    for (int attempt = 0; attempt < 16; ++attempt) {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) {
            break;
        }
        wchar_t guid_text[40]{};
        if (StringFromGUID2(guid, guid_text, static_cast<int>(std::size(guid_text))) <= 0) {
            continue;
        }
        std::wstring name(prefix);
        for (const wchar_t character : std::wstring_view(guid_text)) {
            if (character != L'{' && character != L'}' && character != L'-') {
                name.push_back(character);
            }
        }
        const std::filesystem::path candidate = parent / name;
        if (CreateDirectoryW(candidate.c_str(), &security)) {
            SetFileAttributesW(
                candidate.c_str(),
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                    FILE_ATTRIBUTE_TEMPORARY);
            result = candidate;
            break;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
    }

    LocalFree(descriptor);
    FreeLibrary(advapi);
    if (!result && error) {
        *error = L"无法创建随机 OCR 临时目录。";
    }
    return result;
}

Bitmap resize_bitmap_bilinear_once(
    const Bitmap& source,
    int target_width,
    int target_height) {
    if (source.empty() || target_width <= 0 || target_height <= 0) {
        return {};
    }
    if (source.width == target_width && source.height == target_height) {
        return source;
    }

    struct AxisSample {
        int first{};
        int second{};
        double weight{};
    };
    const auto make_samples = [](int source_size, int target_size) {
        std::vector<AxisSample> samples(static_cast<std::size_t>(target_size));
        const double ratio =
            static_cast<double>(source_size) / static_cast<double>(target_size);
        for (int destination = 0; destination < target_size; ++destination) {
            const double source_position =
                (static_cast<double>(destination) + 0.5) * ratio - 0.5;
            const int lower = static_cast<int>(std::floor(source_position));
            samples[static_cast<std::size_t>(destination)] = {
                std::clamp(lower, 0, source_size - 1),
                std::clamp(lower + 1, 0, source_size - 1),
                std::clamp(source_position - lower, 0.0, 1.0),
            };
        }
        return samples;
    };

    const auto horizontal = make_samples(source.width, target_width);
    const auto vertical = make_samples(source.height, target_height);
    Bitmap target(target_width, target_height);
    if (target.empty()) {
        return {};
    }

    for (int y = 0; y < target_height; ++y) {
        const AxisSample& y_sample = vertical[static_cast<std::size_t>(y)];
        const auto first_row = source.row(y_sample.first);
        const auto second_row = source.row(y_sample.second);
        auto destination_row = target.row(y);
        for (int x = 0; x < target_width; ++x) {
            const AxisSample& x_sample = horizontal[static_cast<std::size_t>(x)];
            for (std::size_t channel = 0; channel < Bitmap::bytes_per_pixel; ++channel) {
                const auto pixel = [channel, &x_sample](std::span<const std::uint8_t> row) {
                    const double first = row[
                        static_cast<std::size_t>(x_sample.first) * Bitmap::bytes_per_pixel +
                        channel];
                    const double second = row[
                        static_cast<std::size_t>(x_sample.second) * Bitmap::bytes_per_pixel +
                        channel];
                    return first + (second - first) * x_sample.weight;
                };
                const double top = pixel(first_row);
                const double bottom = pixel(second_row);
                const double value = top + (bottom - top) * y_sample.weight;
                destination_row[
                    static_cast<std::size_t>(x) * Bitmap::bytes_per_pixel + channel] =
                    static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
            }
        }
    }
    target.make_opaque();
    return target;
}

Bitmap resize_bitmap_high_quality_impl(
    const Bitmap& source,
    int target_width,
    int target_height) {
    if (source.empty() || target_width <= 0 || target_height <= 0) {
        return {};
    }
    if (source.width == target_width && source.height == target_height) {
        return source;
    }

    const Bitmap* current = &source;
    Bitmap intermediate;
    while (current->width > target_width * 2LL ||
           current->height > target_height * 2LL) {
        const int next_width = std::max(target_width, (current->width + 1) / 2);
        const int next_height = std::max(target_height, (current->height + 1) / 2);
        Bitmap next = resize_bitmap_bilinear_once(*current, next_width, next_height);
        if (next.empty()) {
            return {};
        }
        intermediate = std::move(next);
        current = &intermediate;
    }
    return resize_bitmap_bilinear_once(*current, target_width, target_height);
}

double select_preprocess_scale_impl(int width, int height) noexcept {
    if (width <= 0 || height <= 0) {
        return 0.0;
    }
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    const int longest_edge = std::max(width, height);
    double scale = 1.0;
    if (pixels <= 600'000ULL && longest_edge <= 1'200) {
        scale = 2.0;
    } else if (pixels <= 1'500'000ULL && longest_edge <= 1'800) {
        scale = 1.5;
    }

    if (scale > 1.0) {
        const double edge_limit =
            4'096.0 / static_cast<double>(longest_edge);
        const double pixel_limit = std::sqrt(
            (8.0 * 1024.0 * 1024.0) / static_cast<double>(pixels));
        scale = std::max(1.0, std::min({scale, edge_limit, pixel_limit}));
    }

    if (pixels > kMaxOcrPixels || longest_edge > kMaxOcrImageEdge) {
        const double edge_scale =
            static_cast<double>(kMaxOcrImageEdge) / longest_edge;
        const double pixel_scale = std::sqrt(
            static_cast<double>(kMaxOcrPixels) / static_cast<double>(pixels));
        scale = std::min({scale, edge_scale, pixel_scale});
    }
    return std::clamp(scale, 0.01, 2.0);
}

int select_thread_count_impl(
    std::uint64_t pixels,
    unsigned int logical_processors,
    bool accurate_profile) noexcept {
    const int maximum = static_cast<int>(
        std::clamp(logical_processors, 1U, 4U));
    if (maximum == 1) {
        return 1;
    }
    if (pixels < 400'000ULL) {
        return std::min(2, maximum);
    }
    if (pixels >= 8ULL * 1024ULL * 1024ULL ||
        (accurate_profile && pixels >= 4ULL * 1024ULL * 1024ULL)) {
        return maximum;
    }
    if (pixels >= 2ULL * 1024ULL * 1024ULL) {
        return std::min(3, maximum);
    }
    return std::min(2, maximum);
}

bool configure_kill_on_close_job(HANDLE job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    return SetInformationJobObject(
               job,
               JobObjectExtendedLimitInformation,
               &limits,
               sizeof(limits)) != FALSE;
}

bool append_available_output(
    HANDLE pipe,
    std::vector<char>& output,
    std::size_t maximum_bytes,
    bool& limit_exceeded) {
    std::array<char, 4096> temporary{};
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD pipe_error = GetLastError();
            return pipe_error == ERROR_BROKEN_PIPE ||
                   pipe_error == ERROR_PIPE_NOT_CONNECTED;
        }
        if (available == 0) {
            return true;
        }

        DWORD bytes_read = 0;
        const DWORD read_size =
            std::min<DWORD>(available, static_cast<DWORD>(temporary.size()));
        if (!ReadFile(
                pipe,
                temporary.data(),
                read_size,
                &bytes_read,
                nullptr)) {
            const DWORD pipe_error = GetLastError();
            return pipe_error == ERROR_BROKEN_PIPE ||
                   pipe_error == ERROR_PIPE_NOT_CONNECTED;
        }
        if (bytes_read == 0) {
            return true;
        }
        if (bytes_read > maximum_bytes - std::min(output.size(), maximum_bytes)) {
            limit_exceeded = true;
            return false;
        }
        output.insert(
            output.end(),
            temporary.begin(),
            temporary.begin() + bytes_read);
    }
}

OcrOutput run_ocr_process(
    const std::filesystem::path& executable,
    std::wstring command_line,
    const OcrDependencyLease& dependency_lease,
    const OcrProtocolExpectations& expected,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        return {false, {}, L"OCR 已取消。"};
    }
    if (!dependency_lease.valid()) {
        return {
            false,
            {},
            L"OCR 依赖 lease 无效。",
        };
    }

    std::wstring executable_error;
    auto executable_guard =
        open_locked_path(
            executable,
            false,
            &executable_error);
    if (!executable_guard) {
        return {
            false,
            {},
            L"无法锁定 OCR 辅助程序：" +
                executable_error,
        };
    }
    const auto verified_executable =
        final_path_from_handle(
            executable_guard->get(),
            &executable_error);
    if (!verified_executable) {
        return {
            false,
            {},
            L"无法解析 OCR 辅助程序最终路径：" +
                executable_error,
        };
    }

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
        return {false, {}, L"创建 OCR 输出管道失败。"};
    }
    UniqueHandle stdout_read(raw_stdout_read);
    UniqueHandle stdout_write(raw_stdout_write);
    UniqueHandle stderr_read(raw_stderr_read);
    UniqueHandle stderr_write(raw_stderr_write);
    if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        return {false, {}, L"限制 OCR 管道句柄继承失败。"};
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
        return {false, {}, L"创建 OCR 标准输入句柄失败。"};
    }

    std::vector<UniqueHandle> inherited_guards;
    inherited_guards.reserve(
        dependency_lease.handles().size());
    for (const HANDLE source :
         dependency_lease.handles()) {
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(
                GetCurrentProcess(),
                source,
                GetCurrentProcess(),
                &duplicate,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS)) {
            return {
                false,
                {},
                L"复制 OCR 依赖保护句柄失败：" +
                    windows_error_message(
                        GetLastError()),
            };
        }
        inherited_guards.emplace_back(duplicate);
    }

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.get() || !configure_kill_on_close_job(job.get())) {
        return {false, {}, L"创建 OCR 作业对象失败。"};
    }

    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_bytes);
    if (attribute_bytes == 0) {
        return {false, {}, L"初始化 OCR 句柄白名单失败。"};
    }
    std::vector<std::byte> attribute_storage(attribute_bytes);
    auto* attributes =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 2, 0, &attribute_bytes)) {
        return {false, {}, L"初始化 OCR 句柄白名单失败。"};
    }

    std::vector<HANDLE> inherited_handles{
        stdin_null.get(),
        stdout_write.get(),
        stderr_write.get(),
    };
    inherited_handles.reserve(
        inherited_handles.size() +
        inherited_guards.size());
    for (const auto& guard : inherited_guards) {
        inherited_handles.push_back(guard.get());
    }
    if (!UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles.data(),
            inherited_handles.size() *
                sizeof(HANDLE),
            nullptr,
            nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        return {false, {}, L"配置 OCR 句柄白名单失败。"};
    }
    const DWORD64 mitigation_policy =
        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON |
        PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_PREFER_SYSTEM32_ALWAYS_ON;
    if (!UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
            const_cast<DWORD64*>(
                &mitigation_policy),
            sizeof(mitigation_policy),
            nullptr,
            nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        return {
            false,
            {},
            L"配置 OCR 辅助进程缓解策略失败。",
        };
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_null.get();
    startup.StartupInfo.hStdOutput = stdout_write.get();
    startup.StartupInfo.hStdError = stderr_write.get();
    startup.lpAttributeList = attributes;

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        verified_executable->c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        verified_executable->parent_path().c_str(),
        &startup.StartupInfo,
        &process);
    DeleteProcThreadAttributeList(attributes);
    stdout_write.reset();
    stderr_write.reset();
    inherited_guards.clear();
    if (!created) {
        return {
            false,
            {},
            L"无法启动 OCR 辅助进程：" + windows_error_message(GetLastError()),
        };
    }

    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);
    if (!AssignProcessToJobObject(job.get(), process_handle.get())) {
        TerminateProcess(process_handle.get(), 125);
        WaitForSingleObject(process_handle.get(), 5'000);
        return {false, {}, L"无法隔离 OCR 辅助进程。"};
    }
    if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
        TerminateJobObject(job.get(), 125);
        WaitForSingleObject(process_handle.get(), 5'000);
        return {false, {}, L"无法启动 OCR 辅助进程线程。"};
    }

    std::vector<char> protocol_output;
    protocol_output.reserve(4096);
    std::vector<char> diagnostic_output;
    diagnostic_output.reserve(1024);
    const ULONGLONG deadline = GetTickCount64() + kOcrProcessTimeoutMs;
    bool timed_out = false;
    bool cancelled = false;
    bool protocol_limit_exceeded = false;
    bool diagnostic_limit_exceeded = false;
    bool wait_failed = false;
    for (;;) {
        if (stop_token.stop_requested()) {
            cancelled = true;
            TerminateJobObject(job.get(), 122);
            WaitForSingleObject(process_handle.get(), 5'000);
            break;
        }
        const bool stdout_ok = append_available_output(
            stdout_read.get(),
            protocol_output,
            kMaxOcrProcessOutputBytes,
            protocol_limit_exceeded);
        const bool stderr_ok = append_available_output(
            stderr_read.get(),
            diagnostic_output,
            kMaxOcrProcessDiagnosticBytes,
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
                kMaxOcrProcessOutputBytes,
                protocol_limit_exceeded);
            append_available_output(
                stderr_read.get(),
                diagnostic_output,
                kMaxOcrProcessDiagnosticBytes,
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

    if (cancelled) {
        return {false, {}, L"OCR 已取消。"};
    }
    if (timed_out) {
        return {
            false,
            {},
            L"OCR 子进程超过 120 秒，已停止本次识别。",
        };
    }
    if (protocol_limit_exceeded) {
        return {
            false,
            {},
            L"OCR 子进程协议输出超过 8 MiB，已停止本次识别。",
        };
    }
    if (diagnostic_limit_exceeded) {
        return {
            false,
            {},
            L"OCR 子进程诊断输出超过 1 MiB，已停止本次识别。",
        };
    }
    if (wait_failed) {
        return {false, {}, L"等待 OCR 子进程时发生错误。"};
    }

    const std::string protocol_bytes(protocol_output.begin(), protocol_output.end());
    const std::string diagnostic_bytes(diagnostic_output.begin(), diagnostic_output.end());
    const std::wstring decoded_diagnostic = from_utf8(diagnostic_bytes);
    if (!diagnostic_output.empty() && decoded_diagnostic.empty()) {
        return {false, {}, L"OCR 子进程返回了无效的 UTF-8 诊断信息。"};
    }
    if (exit_code != 0) {
        return {
            false,
            {},
            decoded_diagnostic.empty() ? L"OCR 子进程执行失败。" : decoded_diagnostic,
        };
    }
    if (protocol_bytes.empty()) {
        return {false, {}, L"OCR 子进程未返回协议数据。"};
    }
    std::wstring protocol_error;
    auto parsed = parse_ocr_runner_protocol(
        protocol_bytes,
        expected,
        &protocol_error);
    if (!parsed) {
        return {
            false,
            {},
            L"OCR 子进程协议无效：" + protocol_error,
        };
    }
    return std::move(*parsed);
}

std::optional<std::array<std::uint8_t, 32>> sha256_bytes(
    std::span<const std::byte> bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> hash_object;
    std::array<std::uint8_t, 32> digest{};

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0))) {
        return std::nullopt;
    }

    DWORD object_size = 0;
    DWORD result_size = 0;
    NTSTATUS status = BCryptGetProperty(
        algorithm,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size),
        sizeof(object_size),
        &result_size,
        0);
    if (BCRYPT_SUCCESS(status)) {
        DWORD hash_size = 0;
        status = BCryptGetProperty(
            algorithm,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_size),
            sizeof(hash_size),
            &result_size,
            0);
        if (!BCRYPT_SUCCESS(status) || hash_size != digest.size()) {
            status = static_cast<NTSTATUS>(0xC0000001L);
        }
    }
    if (BCRYPT_SUCCESS(status)) {
        hash_object.resize(object_size);
        status = BCryptCreateHash(
            algorithm,
            &hash,
            hash_object.data(),
            static_cast<ULONG>(hash_object.size()),
            nullptr,
            0,
            0);
    }
    if (BCRYPT_SUCCESS(status)) {
        if (bytes.size() > std::numeric_limits<ULONG>::max()) {
            status = static_cast<NTSTATUS>(0xC000000DL);
        } else {
            status = BCryptHashData(
                hash,
                const_cast<PUCHAR>(
                    reinterpret_cast<const UCHAR*>(bytes.data())),
                static_cast<ULONG>(bytes.size()),
                0);
        }
    }
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptFinishHash(
            hash,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            0);
    }

    if (hash) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return std::nullopt;
    }
    return digest;
}

}  // namespace

OcrDependencyLease::OcrDependencyLease(
    std::vector<HANDLE> handles,
    std::uint64_t sequence,
    std::filesystem::path root) noexcept
    : handles_(std::move(handles)),
      sequence_(sequence),
      root_(std::move(root)) {}

OcrDependencyLease::~OcrDependencyLease() {
    reset();
}

OcrDependencyLease::OcrDependencyLease(
    OcrDependencyLease&& other) noexcept
    : handles_(std::move(other.handles_)),
      sequence_(
          std::exchange(other.sequence_, 0)),
      root_(std::move(other.root_)) {
    other.handles_.clear();
    other.root_.clear();
}

OcrDependencyLease&
OcrDependencyLease::operator=(
    OcrDependencyLease&& other) noexcept {
    if (this != &other) {
        reset();
        handles_ = std::move(other.handles_);
        sequence_ =
            std::exchange(other.sequence_, 0);
        root_ = std::move(other.root_);
        other.handles_.clear();
        other.root_.clear();
    }
    return *this;
}

bool OcrDependencyLease::valid() const noexcept {
    return sequence_ != 0 &&
           !handles_.empty() &&
           !root_.empty();
}

std::uint64_t
OcrDependencyLease::sequence() const noexcept {
    return sequence_;
}

const std::filesystem::path&
OcrDependencyLease::root() const noexcept {
    return root_;
}

std::span<const HANDLE>
OcrDependencyLease::handles() const noexcept {
    return handles_;
}

void OcrDependencyLease::reset() noexcept {
    for (const HANDLE handle : handles_) {
        if (handle &&
            handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
    handles_.clear();
    sequence_ = 0;
    root_.clear();
}

std::uint64_t ocr_minimum_sequence() noexcept {
    return kCompiledMinimumSequence;
}

namespace {

class StrictJsonScanner {
public:
    explicit StrictJsonScanner(std::wstring_view text) noexcept
        : text_(text) {}

    [[nodiscard]] bool valid() {
        skip_whitespace();
        if (!parse_value(0)) {
            return false;
        }
        skip_whitespace();
        return position_ == text_.size();
    }

private:
    [[nodiscard]] static int hex_value(wchar_t character) noexcept {
        if (character >= L'0' && character <= L'9') {
            return character - L'0';
        }
        if (character >= L'a' && character <= L'f') {
            return character - L'a' + 10;
        }
        if (character >= L'A' && character <= L'F') {
            return character - L'A' + 10;
        }
        return -1;
    }

    void skip_whitespace() noexcept {
        while (position_ < text_.size() &&
               (text_[position_] == L' ' || text_[position_] == L'\t' ||
                text_[position_] == L'\r' || text_[position_] == L'\n')) {
            ++position_;
        }
    }

    [[nodiscard]] bool consume(std::wstring_view token) noexcept {
        if (text_.substr(position_, token.size()) != token) {
            return false;
        }
        position_ += token.size();
        return true;
    }

    [[nodiscard]] bool parse_string(std::wstring* decoded) {
        if (position_ >= text_.size() || text_[position_] != L'"') {
            return false;
        }
        ++position_;
        while (position_ < text_.size()) {
            const wchar_t character = text_[position_++];
            if (character == L'"') {
                return true;
            }
            if (character < L' ') {
                return false;
            }
            if (character != L'\\') {
                if (decoded) {
                    decoded->push_back(character);
                }
                continue;
            }
            if (position_ >= text_.size()) {
                return false;
            }
            const wchar_t escaped = text_[position_++];
            wchar_t value = 0;
            switch (escaped) {
                case L'"': value = L'"'; break;
                case L'\\': value = L'\\'; break;
                case L'/': value = L'/'; break;
                case L'b': value = L'\b'; break;
                case L'f': value = L'\f'; break;
                case L'n': value = L'\n'; break;
                case L'r': value = L'\r'; break;
                case L't': value = L'\t'; break;
                case L'u': {
                    if (text_.size() - position_ < 4) {
                        return false;
                    }
                    unsigned int code_unit = 0;
                    for (int index = 0; index < 4; ++index) {
                        const int digit = hex_value(text_[position_++]);
                        if (digit < 0) {
                            return false;
                        }
                        code_unit = (code_unit << 4U) | static_cast<unsigned int>(digit);
                    }
                    value = static_cast<wchar_t>(code_unit);
                    break;
                }
                default: return false;
            }
            if (decoded) {
                decoded->push_back(value);
            }
        }
        return false;
    }

    [[nodiscard]] bool parse_object(std::size_t depth) {
        ++position_;
        skip_whitespace();
        if (position_ < text_.size() && text_[position_] == L'}') {
            ++position_;
            return true;
        }

        std::set<std::wstring> keys;
        for (;;) {
            std::wstring key;
            if (!parse_string(&key) || !keys.insert(std::move(key)).second) {
                return false;
            }
            skip_whitespace();
            if (position_ >= text_.size() || text_[position_++] != L':') {
                return false;
            }
            skip_whitespace();
            if (!parse_value(depth + 1)) {
                return false;
            }
            skip_whitespace();
            if (position_ >= text_.size()) {
                return false;
            }
            if (text_[position_] == L'}') {
                ++position_;
                return true;
            }
            if (text_[position_++] != L',') {
                return false;
            }
            skip_whitespace();
        }
    }

    [[nodiscard]] bool parse_array(std::size_t depth) {
        ++position_;
        skip_whitespace();
        if (position_ < text_.size() && text_[position_] == L']') {
            ++position_;
            return true;
        }
        for (;;) {
            if (!parse_value(depth + 1)) {
                return false;
            }
            skip_whitespace();
            if (position_ >= text_.size()) {
                return false;
            }
            if (text_[position_] == L']') {
                ++position_;
                return true;
            }
            if (text_[position_++] != L',') {
                return false;
            }
            skip_whitespace();
        }
    }

    [[nodiscard]] bool parse_number() noexcept {
        const std::size_t start = position_;
        while (position_ < text_.size()) {
            const wchar_t character = text_[position_];
            if ((character >= L'0' && character <= L'9') ||
                character == L'-' || character == L'+' || character == L'.' ||
                character == L'e' || character == L'E') {
                ++position_;
            } else {
                break;
            }
        }
        return position_ > start;
    }

    [[nodiscard]] bool parse_value(std::size_t depth) {
        if (depth > 64 || position_ >= text_.size()) {
            return false;
        }
        switch (text_[position_]) {
            case L'{': return parse_object(depth);
            case L'[': return parse_array(depth);
            case L'"': return parse_string(nullptr);
            case L't': return consume(L"true");
            case L'f': return consume(L"false");
            case L'n': return consume(L"null");
            default:
                return (text_[position_] == L'-' ||
                        (text_[position_] >= L'0' && text_[position_] <= L'9')) &&
                       parse_number();
        }
    }

    std::wstring_view text_;
    std::size_t position_{};
};

}  // namespace

std::optional<OcrOutput> parse_ocr_runner_protocol(
    std::string_view json_utf8,
    const OcrProtocolExpectations& expected,
    std::wstring* error) {
    if (error) {
        error->clear();
    }
    const auto fail = [error](std::wstring_view reason) -> std::optional<OcrOutput> {
        if (error) {
            *error = reason;
        }
        return std::nullopt;
    };
    if (json_utf8.empty() || json_utf8.size() > kMaxOcrProcessOutputBytes) {
        return fail(L"协议为空或超过大小上限。");
    }
    const std::wstring json = from_utf8(json_utf8);
    if (json.empty()) {
        return fail(L"协议不是合法 UTF-8。");
    }
    StrictJsonScanner scanner(json);
    if (!scanner.valid()) {
        return fail(L"协议 JSON 结构无效或包含重复字段。");
    }

    try {
        const ScopedWinrtApartment apartment;
        const auto root = winrt::Windows::Data::Json::JsonObject::Parse(json);
        if (root.Size() != 5 || !root.HasKey(L"schemaVersion") ||
            !root.HasKey(L"profile") || !root.HasKey(L"preprocess") ||
            !root.HasKey(L"timings") || !root.HasKey(L"blocks")) {
            return fail(L"协议顶层字段不符合 schemaVersion 1。");
        }

        const auto exact_integer = [](
                                       const auto& object,
                                       std::wstring_view name,
                                       int minimum,
                                       int maximum) -> std::optional<int> {
            const double value = object.GetNamedNumber(winrt::hstring(name));
            if (!std::isfinite(value) || std::trunc(value) != value ||
                value < static_cast<double>(minimum) ||
                value > static_cast<double>(maximum)) {
                return std::nullopt;
            }
            return static_cast<int>(value);
        };
        const auto finite_number = [](
                                       const auto& object,
                                       std::wstring_view name,
                                       double minimum,
                                       double maximum) -> std::optional<double> {
            const double value = object.GetNamedNumber(winrt::hstring(name));
            if (!std::isfinite(value) || value < minimum || value > maximum) {
                return std::nullopt;
            }
            return value;
        };

        const auto schema_version = exact_integer(
            root,
            L"schemaVersion",
            1,
            static_cast<int>(kOcrProtocolSchemaVersion));
        if (!schema_version ||
            *schema_version != static_cast<int>(kOcrProtocolSchemaVersion)) {
            return fail(L"不支持的 OCR 协议版本。");
        }

        OcrOutput output;
        output.profile = root.GetNamedString(L"profile").c_str();
        const bool supported_profile =
            output.profile == kOcrEngineRapidV5Fast ||
            output.profile == kOcrEngineRapidV5Accurate ||
            output.profile == kOcrEngineRapidV4Compat;
        if (!supported_profile ||
            (!expected.profile.empty() && output.profile != expected.profile)) {
            return fail(L"OCR 协议 profile 与请求不匹配。");
        }

        const auto preprocess = root.GetNamedObject(L"preprocess");
        if (preprocess.Size() != 12 ||
            !preprocess.HasKey(L"sourceWidth") ||
            !preprocess.HasKey(L"sourceHeight") ||
            !preprocess.HasKey(L"inputWidth") ||
            !preprocess.HasKey(L"inputHeight") ||
            !preprocess.HasKey(L"scaleX") ||
            !preprocess.HasKey(L"scaleY") ||
            !preprocess.HasKey(L"resample") ||
            !preprocess.HasKey(L"tiled") ||
            !preprocess.HasKey(L"tileCount") ||
            !preprocess.HasKey(L"tileSize") ||
            !preprocess.HasKey(L"tileOverlap") ||
            !preprocess.HasKey(L"coordinateSpace")) {
            return fail(L"OCR 预处理字段不完整或包含未知字段。");
        }
        const auto source_width = exact_integer(
            preprocess, L"sourceWidth", 1, kMaxOcrProtocolDimension);
        const auto source_height = exact_integer(
            preprocess, L"sourceHeight", 1, kMaxOcrProtocolDimension);
        const auto input_width = exact_integer(
            preprocess, L"inputWidth", 1, kMaxOcrProtocolDimension);
        const auto input_height = exact_integer(
            preprocess, L"inputHeight", 1, kMaxOcrProtocolDimension);
        const auto scale_x = finite_number(preprocess, L"scaleX", 0.01, 2.0);
        const auto scale_y = finite_number(preprocess, L"scaleY", 0.01, 2.0);
        const auto tile_count = exact_integer(preprocess, L"tileCount", 1, 512);
        const auto tile_size = exact_integer(preprocess, L"tileSize", 0, 4'096);
        const auto tile_overlap = exact_integer(preprocess, L"tileOverlap", 0, 1'024);
        if (!source_width || !source_height || !input_width || !input_height ||
            !scale_x || !scale_y || !tile_count || !tile_size || !tile_overlap) {
            return fail(L"OCR 预处理数值越界。");
        }
        const std::wstring resample = preprocess.GetNamedString(L"resample").c_str();
        const std::wstring coordinate_space =
            preprocess.GetNamedString(L"coordinateSpace").c_str();
        const bool tiled = preprocess.GetNamedBoolean(L"tiled");
        if ((resample != L"none" && resample != L"bilinear-upscale" &&
             resample != L"progressive-bilinear-downscale") ||
            coordinate_space != L"input-pixels") {
            return fail(L"OCR 预处理模式或坐标空间无效。");
        }
        const double calculated_scale_x =
            static_cast<double>(*input_width) / static_cast<double>(*source_width);
        const double calculated_scale_y =
            static_cast<double>(*input_height) / static_cast<double>(*source_height);
        if (std::abs(*scale_x - calculated_scale_x) > 1.0e-9 ||
            std::abs(*scale_y - calculated_scale_y) > 1.0e-9) {
            return fail(L"OCR 预处理缩放比例不自洽。");
        }
        if ((!tiled && (*tile_count != 1 || *tile_size != 0 || *tile_overlap != 0)) ||
            (tiled && (*tile_count < 2 || *tile_size < 512 || *tile_overlap < 32 ||
                       *tile_overlap >= *tile_size / 2))) {
            return fail(L"OCR 分块元数据不自洽。");
        }
        if ((expected.source_width > 0 && *source_width != expected.source_width) ||
            (expected.source_height > 0 && *source_height != expected.source_height) ||
            (expected.input_width > 0 && *input_width != expected.input_width) ||
            (expected.input_height > 0 && *input_height != expected.input_height) ||
            (expected.scale_x > 0.0 && std::abs(*scale_x - expected.scale_x) > 1.0e-9) ||
            (expected.scale_y > 0.0 && std::abs(*scale_y - expected.scale_y) > 1.0e-9) ||
            (!expected.resample.empty() && resample != expected.resample)) {
            return fail(L"OCR 预处理元数据与请求不匹配。");
        }
        output.preprocess = {
            *source_width,
            *source_height,
            *input_width,
            *input_height,
            *scale_x,
            *scale_y,
            resample,
            tiled,
            *tile_count,
            *tile_size,
            *tile_overlap,
        };

        const auto timings = root.GetNamedObject(L"timings");
        if (timings.Size() != 5 || !timings.HasKey(L"decodeMs") ||
            !timings.HasKey(L"modelInitMs") ||
            !timings.HasKey(L"inferenceMs") ||
            !timings.HasKey(L"mergeMs") || !timings.HasKey(L"totalMs")) {
            return fail(L"OCR timing 字段不完整或包含未知字段。");
        }
        const auto decode_ms = finite_number(
            timings, L"decodeMs", 0.0, kMaxOcrProtocolTimingMs);
        const auto model_init_ms = finite_number(
            timings, L"modelInitMs", 0.0, kMaxOcrProtocolTimingMs);
        const auto inference_ms = finite_number(
            timings, L"inferenceMs", 0.0, kMaxOcrProtocolTimingMs);
        const auto merge_ms = finite_number(
            timings, L"mergeMs", 0.0, kMaxOcrProtocolTimingMs);
        const auto total_ms = finite_number(
            timings, L"totalMs", 0.0, kMaxOcrProtocolTimingMs);
        if (!decode_ms || !model_init_ms || !inference_ms || !merge_ms || !total_ms ||
            *total_ms + 0.001 < std::max({
                *decode_ms, *model_init_ms, *inference_ms, *merge_ms})) {
            return fail(L"OCR timing 数值无效。");
        }
        output.timings = {
            *decode_ms,
            *model_init_ms,
            *inference_ms,
            *merge_ms,
            *total_ms,
        };

        const auto blocks = root.GetNamedArray(L"blocks");
        if (blocks.Size() > kMaxOcrProtocolBlocks) {
            return fail(L"OCR 文字块数量超过上限。");
        }
        output.blocks.reserve(blocks.Size());
        std::size_t total_text_characters = 0;
        for (const auto& value : blocks) {
            const auto block_object = value.GetObject();
            if (block_object.Size() != 3 || !block_object.HasKey(L"quad") ||
                !block_object.HasKey(L"text") || !block_object.HasKey(L"score")) {
                return fail(L"OCR 文字块字段无效。");
            }
            std::wstring text = block_object.GetNamedString(L"text").c_str();
            const auto first_non_space = std::ranges::find_if_not(
                text,
                [](wchar_t character) { return std::iswspace(character) != 0; });
            const auto last_non_space = std::find_if_not(
                text.rbegin(),
                text.rend(),
                [](wchar_t character) { return std::iswspace(character) != 0; });
            if (first_non_space == text.end()) {
                return fail(L"OCR 文字块包含空文本。");
            }
            text = std::wstring(first_non_space, last_non_space.base());
            if (text.size() > kMaxOcrBlockTextCharacters ||
                total_text_characters > kMaxOcrTotalTextCharacters - text.size() ||
                std::ranges::any_of(text, [](wchar_t character) {
                    return character < L' ' || character == 0x7f;
                }) ||
                to_utf8(text).empty()) {
                return fail(L"OCR 文字块文本无效或超过上限。");
            }
            total_text_characters += text.size();
            const double score = block_object.GetNamedNumber(L"score");
            if (!std::isfinite(score) || score < 0.0 || score > 1.0) {
                return fail(L"OCR 文字块置信度无效。");
            }

            const auto quad = block_object.GetNamedArray(L"quad");
            if (quad.Size() != 4) {
                return fail(L"OCR 文字块 quad 必须包含四个点。");
            }
            OcrBlock block;
            block.text = std::move(text);
            block.score = score;
            double left = std::numeric_limits<double>::max();
            double top = std::numeric_limits<double>::max();
            double right = std::numeric_limits<double>::lowest();
            double bottom = std::numeric_limits<double>::lowest();
            for (std::uint32_t point_index = 0; point_index < 4; ++point_index) {
                const auto point = quad.GetAt(point_index).GetArray();
                if (point.Size() != 2) {
                    return fail(L"OCR quad 点坐标格式无效。");
                }
                const double input_x = point.GetAt(0).GetNumber();
                const double input_y = point.GetAt(1).GetNumber();
                if (!std::isfinite(input_x) || !std::isfinite(input_y) ||
                    input_x < 0.0 || input_y < 0.0 ||
                    input_x > static_cast<double>(*input_width) ||
                    input_y > static_cast<double>(*input_height)) {
                    return fail(L"OCR quad 坐标越界或不是有限数值。");
                }
                const double source_x = std::clamp(
                    input_x / *scale_x,
                    0.0,
                    static_cast<double>(*source_width));
                const double source_y = std::clamp(
                    input_y / *scale_y,
                    0.0,
                    static_cast<double>(*source_height));
                block.quad[point_index] = {source_x, source_y};
                left = std::min(left, source_x);
                top = std::min(top, source_y);
                right = std::max(right, source_x);
                bottom = std::max(bottom, source_y);
            }
            if (right - left < 0.25 || bottom - top < 0.25) {
                return fail(L"OCR quad 退化为无效区域。");
            }
            output.blocks.push_back(std::move(block));
        }

        const auto bounds = [](const OcrBlock& block) {
            std::array<double, 6> result{
                std::numeric_limits<double>::max(),
                std::numeric_limits<double>::max(),
                std::numeric_limits<double>::lowest(),
                std::numeric_limits<double>::lowest(),
                0.0,
                0.0,
            };
            for (const auto& point : block.quad) {
                result[0] = std::min(result[0], point.x);
                result[1] = std::min(result[1], point.y);
                result[2] = std::max(result[2], point.x);
                result[3] = std::max(result[3], point.y);
            }
            result[4] = (result[1] + result[3]) * 0.5;
            result[5] = result[3] - result[1];
            return result;
        };
        std::vector<std::size_t> order(output.blocks.size());
        for (std::size_t index = 0; index < order.size(); ++index) {
            order[index] = index;
        }
        std::ranges::sort(order, [&output, &bounds](std::size_t first, std::size_t second) {
            const auto first_bounds = bounds(output.blocks[first]);
            const auto second_bounds = bounds(output.blocks[second]);
            if (first_bounds[4] != second_bounds[4]) {
                return first_bounds[4] < second_bounds[4];
            }
            if (first_bounds[0] != second_bounds[0]) {
                return first_bounds[0] < second_bounds[0];
            }
            if (first_bounds[1] != second_bounds[1]) {
                return first_bounds[1] < second_bounds[1];
            }
            return output.blocks[first].text < output.blocks[second].text;
        });

        struct TextLine {
            std::vector<std::size_t> blocks;
            double center_sum{};
            double height_sum{};
            double top{std::numeric_limits<double>::max()};
            double left{std::numeric_limits<double>::max()};
        };
        std::vector<TextLine> lines;
        for (const std::size_t block_index : order) {
            const auto block_bounds = bounds(output.blocks[block_index]);
            std::optional<std::size_t> best_line;
            if (!lines.empty()) {
                const auto& line = lines.back();
                const double count = static_cast<double>(line.blocks.size());
                const double line_center = line.center_sum / count;
                const double line_height = line.height_sum / count;
                const double distance = std::abs(block_bounds[4] - line_center);
                if (distance <=
                    std::max(2.0, 0.55 * std::max(block_bounds[5], line_height))) {
                    best_line = lines.size() - 1;
                }
            }
            if (!best_line) {
                lines.push_back({
                    {block_index},
                    block_bounds[4],
                    block_bounds[5],
                    block_bounds[1],
                    block_bounds[0],
                });
            } else {
                auto& line = lines[*best_line];
                line.blocks.push_back(block_index);
                line.center_sum += block_bounds[4];
                line.height_sum += block_bounds[5];
                line.top = std::min(line.top, block_bounds[1]);
                line.left = std::min(line.left, block_bounds[0]);
            }
        }
        std::ranges::sort(lines, [](const TextLine& first, const TextLine& second) {
            if (first.top != second.top) {
                return first.top < second.top;
            }
            return first.left < second.left;
        });

        std::vector<OcrBlock> sorted_blocks;
        sorted_blocks.reserve(output.blocks.size());
        std::vector<std::wstring> text_lines;
        text_lines.reserve(lines.size());
        for (auto& line : lines) {
            std::ranges::sort(line.blocks, [&output, &bounds](std::size_t first, std::size_t second) {
                const auto first_bounds = bounds(output.blocks[first]);
                const auto second_bounds = bounds(output.blocks[second]);
                if (first_bounds[0] != second_bounds[0]) {
                    return first_bounds[0] < second_bounds[0];
                }
                if (first_bounds[4] != second_bounds[4]) {
                    return first_bounds[4] < second_bounds[4];
                }
                return output.blocks[first].text < output.blocks[second].text;
            });
            std::wstring line_text;
            for (const std::size_t block_index : line.blocks) {
                if (!line_text.empty()) {
                    line_text.push_back(L' ');
                }
                line_text += output.blocks[block_index].text;
                sorted_blocks.push_back(std::move(output.blocks[block_index]));
            }
            text_lines.push_back(std::move(line_text));
        }
        output.blocks = std::move(sorted_blocks);
        output.text = join_ocr_lines(text_lines);
        output.ok = !output.text.empty();
        if (!output.ok) {
            output.error = L"OCR 未识别到任何文本。";
        }
        return output;
    } catch (...) {
        return fail(L"OCR JSON 结构或字段类型无效。");
    }
}

namespace {

struct WarmWorkerResponse {
    bool ok{};
    OcrOutput output;
    std::wstring error;
};

std::optional<WarmWorkerResponse> parse_warm_worker_response(
    std::string_view response_utf8,
    std::uint64_t expected_request_id,
    std::wstring_view expected_profile,
    const std::filesystem::path& expected_root,
    std::uint64_t expected_sequence,
    const OcrProtocolExpectations& expected_image,
    std::wstring* protocol_error) {
    const auto fail = [protocol_error](std::wstring_view reason)
        -> std::optional<WarmWorkerResponse> {
        if (protocol_error) {
            *protocol_error = reason;
        }
        return std::nullopt;
    };
    if (protocol_error) {
        protocol_error->clear();
    }
    if (response_utf8.empty() || response_utf8.size() > kMaxWarmWorkerResponseBytes) {
        return fail(L"温热 worker 响应为空或超过上限。");
    }
    const std::wstring response_json = from_utf8(response_utf8);
    if (response_json.empty()) {
        return fail(L"温热 worker 响应不是合法 UTF-8。");
    }
    StrictJsonScanner scanner(response_json);
    if (!scanner.valid()) {
        return fail(L"温热 worker 响应 JSON 无效或包含重复字段。");
    }

    try {
        const ScopedWinrtApartment apartment;
        const auto root = winrt::Windows::Data::Json::JsonObject::Parse(response_json);
        const bool has_result = root.HasKey(L"result");
        const bool has_error = root.HasKey(L"error");
        if (root.Size() != 7 || !root.HasKey(L"schemaVersion") ||
            !root.HasKey(L"requestId") || !root.HasKey(L"profile") ||
            !root.HasKey(L"dependencyRoot") ||
            !root.HasKey(L"dependencySequence") || !root.HasKey(L"ok") ||
            has_result == has_error) {
            return fail(L"温热 worker 响应字段不符合 schema。");
        }
        const auto exact_uint64 = [&root](
                                      std::wstring_view name,
                                      std::uint64_t minimum,
                                      std::uint64_t maximum)
            -> std::optional<std::uint64_t> {
            const double value = root.GetNamedNumber(winrt::hstring(name));
            if (!std::isfinite(value) || std::trunc(value) != value ||
                value < static_cast<double>(minimum) ||
                value > static_cast<double>(maximum)) {
                return std::nullopt;
            }
            return static_cast<std::uint64_t>(value);
        };
        const auto schema = exact_uint64(
            L"schemaVersion", 1, kWarmWorkerSchemaVersion);
        const auto request_id = exact_uint64(
            L"requestId", 1, kMaxSafeJsonInteger);
        const auto sequence = exact_uint64(
            L"dependencySequence", 1, kMaxSafeJsonInteger);
        const std::wstring profile = root.GetNamedString(L"profile").c_str();
        const std::wstring dependency_root =
            root.GetNamedString(L"dependencyRoot").c_str();
        const bool ok = root.GetNamedBoolean(L"ok");
        if (!schema || *schema != kWarmWorkerSchemaVersion ||
            !request_id || *request_id != expected_request_id ||
            !sequence || *sequence != expected_sequence ||
            profile != expected_profile ||
            dependency_root != expected_root.wstring() || ok != has_result) {
            return fail(L"温热 worker 响应身份与当前请求不匹配。");
        }

        WarmWorkerResponse response;
        response.ok = ok;
        if (!ok) {
            response.error = root.GetNamedString(L"error").c_str();
            if (response.error.empty() || response.error.size() > 4'096 ||
                std::ranges::any_of(response.error, [](wchar_t character) {
                    return character < L' ' || character == 0x7f;
                }) ||
                to_utf8(response.error).empty()) {
                return fail(L"温热 worker 错误字段无效。");
            }
            return response;
        }

        const std::wstring result_json =
            root.GetNamedObject(L"result").Stringify().c_str();
        const std::string result_utf8 = to_utf8(result_json);
        if (result_utf8.empty()) {
            return fail(L"温热 worker 嵌套结果无法编码为 UTF-8。");
        }
        std::wstring result_error;
        auto parsed_result = parse_ocr_runner_protocol(
            result_utf8,
            expected_image,
            &result_error);
        if (!parsed_result) {
            return fail(L"温热 worker 嵌套结果无效：" + result_error);
        }
        response.output = std::move(*parsed_result);
        return response;
    } catch (...) {
        return fail(L"温热 worker 响应字段类型无效。");
    }
}

bool warm_equals_ignore_case(std::wstring_view first, std::wstring_view second) {
    return first.size() == second.size() &&
           CompareStringOrdinal(
               first.data(),
               static_cast<int>(first.size()),
               second.data(),
               static_cast<int>(second.size()),
               TRUE) == CSTR_EQUAL;
}

bool warm_starts_with_ignore_case(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
           CompareStringOrdinal(
               value.data(),
               static_cast<int>(prefix.size()),
               prefix.data(),
               static_cast<int>(prefix.size()),
               TRUE) == CSTR_EQUAL;
}

bool warm_remove_environment_entry(std::wstring_view entry) {
    const std::size_t search_from = !entry.empty() && entry.front() == L'=' ? 1U : 0U;
    const std::size_t separator = entry.find(L'=', search_from);
    if (separator == std::wstring_view::npos) {
        return false;
    }
    const std::wstring_view name = entry.substr(0, separator);
    return warm_starts_with_ignore_case(name, L"PYTHON") ||
           warm_starts_with_ignore_case(name, L"_PYI_") ||
           warm_equals_ignore_case(name, L"_MEIPASS2") ||
           warm_equals_ignore_case(name, L"PYINSTALLER_RESET_ENVIRONMENT");
}

std::optional<std::vector<wchar_t>> warm_runner_environment_block() {
    LPWCH raw_environment = GetEnvironmentStringsW();
    if (!raw_environment) {
        return std::nullopt;
    }
    try {
        std::vector<std::wstring> entries;
        for (const wchar_t* cursor = raw_environment; *cursor != L'\0';) {
            const std::wstring_view entry(cursor);
            if (!warm_remove_environment_entry(entry)) {
                entries.emplace_back(entry);
            }
            cursor += entry.size() + 1U;
        }
        FreeEnvironmentStringsW(raw_environment);
        raw_environment = nullptr;
        entries.emplace_back(L"PYINSTALLER_RESET_ENVIRONMENT=1");
        std::ranges::sort(entries, [](const std::wstring& first, const std::wstring& second) {
            const int insensitive = CompareStringOrdinal(
                first.c_str(), static_cast<int>(first.size()),
                second.c_str(), static_cast<int>(second.size()), TRUE);
            if (insensitive != CSTR_EQUAL) {
                return insensitive == CSTR_LESS_THAN;
            }
            return CompareStringOrdinal(
                       first.c_str(), static_cast<int>(first.size()),
                       second.c_str(), static_cast<int>(second.size()), FALSE) == CSTR_LESS_THAN;
        });
        std::size_t characters = 1;
        for (const auto& entry : entries) {
            if (entry.size() > std::numeric_limits<std::size_t>::max() - characters - 1U) {
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

std::filesystem::path warm_system_directory() {
    std::wstring buffer(32'768, L'\0');
    const UINT size = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (size == 0 || size >= buffer.size()) {
        return {};
    }
    buffer.resize(size);
    return std::filesystem::path(buffer);
}

std::optional<std::wstring> warm_current_dll_directory() {
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
            static_cast<DWORD>(directory.size()), directory.data());
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

bool warm_paths_equal(
    const std::filesystem::path& first,
    const std::filesystem::path& second) {
    const std::wstring first_text = first.lexically_normal().generic_wstring();
    const std::wstring second_text = second.lexically_normal().generic_wstring();
    return warm_equals_ignore_case(first_text, second_text);
}

std::optional<std::string> make_warm_worker_request(
    std::uint64_t request_id,
    std::wstring_view profile,
    const std::filesystem::path& dependency_root,
    std::uint64_t dependency_sequence,
    const std::filesystem::path& image_path,
    const OcrProtocolExpectations& image) {
    try {
        using winrt::Windows::Data::Json::JsonObject;
        using winrt::Windows::Data::Json::JsonValue;
        const ScopedWinrtApartment apartment;
        JsonObject image_object;
        image_object.Insert(L"path", JsonValue::CreateStringValue(image_path.wstring()));
        image_object.Insert(L"sourceWidth", JsonValue::CreateNumberValue(image.source_width));
        image_object.Insert(L"sourceHeight", JsonValue::CreateNumberValue(image.source_height));
        image_object.Insert(L"inputWidth", JsonValue::CreateNumberValue(image.input_width));
        image_object.Insert(L"inputHeight", JsonValue::CreateNumberValue(image.input_height));
        image_object.Insert(L"scaleX", JsonValue::CreateNumberValue(image.scale_x));
        image_object.Insert(L"scaleY", JsonValue::CreateNumberValue(image.scale_y));
        image_object.Insert(L"resample", JsonValue::CreateStringValue(image.resample));

        JsonObject root;
        root.Insert(
            L"schemaVersion",
            JsonValue::CreateNumberValue(static_cast<double>(kWarmWorkerSchemaVersion)));
        root.Insert(
            L"requestId", JsonValue::CreateNumberValue(static_cast<double>(request_id)));
        root.Insert(L"profile", JsonValue::CreateStringValue(profile));
        root.Insert(
            L"dependencyRoot", JsonValue::CreateStringValue(dependency_root.wstring()));
        root.Insert(
            L"dependencySequence",
            JsonValue::CreateNumberValue(static_cast<double>(dependency_sequence)));
        root.Insert(L"image", image_object);
        const std::string result = to_utf8(root.Stringify().c_str());
        if (result.empty() || result.size() > kMaxWarmWorkerRequestBytes) {
            return std::nullopt;
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

namespace {

enum class WarmAttemptStatus {
    completed,
    cancelled,
    unavailable,
};

bool warm_worker_key_matches_impl(
    bool process_healthy,
    const std::filesystem::path& current_root,
    std::uint64_t current_sequence,
    std::wstring_view current_profile,
    int current_ort_threads,
    const std::filesystem::path& requested_root,
    std::uint64_t requested_sequence,
    std::wstring_view requested_profile,
    int requested_ort_threads) {
    return process_healthy && current_sequence == requested_sequence &&
           current_profile == requested_profile &&
           current_ort_threads == requested_ort_threads &&
           warm_paths_equal(current_root, requested_root);
}

bool warm_attempt_allows_fallback(
    WarmAttemptStatus status,
    bool stop_requested) noexcept {
    return status == WarmAttemptStatus::unavailable && !stop_requested;
}

std::optional<std::uint32_t> decode_warm_frame_size(
    std::span<const char> header,
    std::size_t maximum) noexcept {
    if (header.size() < 4U) {
        return std::nullopt;
    }
    const auto byte = [&header](std::size_t index) {
        return static_cast<std::uint32_t>(
            static_cast<unsigned char>(header[index]));
    };
    const std::uint32_t size =
        byte(0) | (byte(1) << 8U) | (byte(2) << 16U) | (byte(3) << 24U);
    if (size == 0 || size > maximum) {
        return std::nullopt;
    }
    return size;
}

struct WarmAttempt {
    WarmAttemptStatus status{WarmAttemptStatus::unavailable};
    OcrOutput output;
    std::wstring error;
};

class WarmOcrWorkerManager {
public:
    ~WarmOcrWorkerManager() {
        std::scoped_lock lock(mutex_);
        discard_locked(125);
    }

    WarmOcrWorkerManager(const WarmOcrWorkerManager&) = delete;
    WarmOcrWorkerManager& operator=(const WarmOcrWorkerManager&) = delete;

    static WarmOcrWorkerManager& instance() {
        // The kernel closes the Job handle and kills the worker when the host
        // exits. Intentionally avoid a blocking static destructor racing a
        // still-unwinding OCR thread during process shutdown.
        static WarmOcrWorkerManager* const manager = new WarmOcrWorkerManager();
        return *manager;
    }

    WarmAttempt recognize(
        const OcrDependencyLease& dependency_lease,
        std::wstring_view profile,
        const std::filesystem::path& model_directory,
        const std::filesystem::path& image_path,
        const OcrProtocolExpectations& image,
        int ort_threads,
        std::stop_token stop_token) {
        std::unique_lock lock(mutex_, std::defer_lock);
        while (!lock.try_lock()) {
            if (stop_token.stop_requested()) {
                return {WarmAttemptStatus::cancelled, {}, L"OCR 已取消。"};
            }
            Sleep(5);
        }
        if (stop_token.stop_requested()) {
            return {WarmAttemptStatus::cancelled, {}, L"OCR 已取消。"};
        }
        if (!dependency_lease.valid()) {
            return {WarmAttemptStatus::unavailable, {}, L"温热 OCR 依赖 lease 无效。"};
        }
        const int effective_ort_threads =
            std::clamp(ort_threads, 1, 4);

        if (!worker_healthy_locked() ||
            !key_matches_locked(
                dependency_lease.root(),
                dependency_lease.sequence(),
                profile,
                effective_ort_threads)) {
            discard_locked(125);
        }
        if (!process_.get()) {
            std::wstring start_error;
            if (!start_locked(
                    dependency_lease,
                    profile,
                    model_directory,
                    effective_ort_threads,
                    &start_error)) {
                discard_locked(125);
                return {
                    WarmAttemptStatus::unavailable,
                    {},
                    start_error.empty() ? L"温热 OCR worker 启动失败。" : start_error,
                };
            }
        }

        std::vector<char> unexpected_stdout;
        std::vector<char> startup_diagnostic;
        bool stdout_limit = false;
        bool diagnostic_limit = false;
        if (!append_available_output(
                stdout_read_.get(),
                unexpected_stdout,
                kMaxWarmWorkerResponseBytes + 4U,
                stdout_limit) ||
            !append_available_output(
                stderr_read_.get(),
                startup_diagnostic,
                kMaxOcrProcessDiagnosticBytes,
                diagnostic_limit) ||
            stdout_limit || diagnostic_limit || !unexpected_stdout.empty()) {
            discard_locked(125);
            return {
                WarmAttemptStatus::unavailable,
                {},
                L"温热 OCR worker 在请求前输出了非预期数据。",
            };
        }

        const std::uint64_t request_id = next_request_id();
        const auto request = make_warm_worker_request(
            request_id,
            profile,
            dependency_lease.root(),
            dependency_lease.sequence(),
            image_path,
            image);
        if (!request) {
            discard_locked(125);
            return {
                WarmAttemptStatus::unavailable,
                {},
                L"无法编码温热 OCR worker 请求。",
            };
        }
        std::vector<std::byte> frame(4U + request->size());
        const std::uint32_t request_size = static_cast<std::uint32_t>(request->size());
        frame[0] = static_cast<std::byte>(request_size & 0xffU);
        frame[1] = static_cast<std::byte>((request_size >> 8U) & 0xffU);
        frame[2] = static_cast<std::byte>((request_size >> 16U) & 0xffU);
        frame[3] = static_cast<std::byte>((request_size >> 24U) & 0xffU);
        std::memcpy(frame.data() + 4U, request->data(), request->size());
        if (!write_all_locked(frame)) {
            discard_locked(125);
            return {
                WarmAttemptStatus::unavailable,
                {},
                L"温热 OCR worker 请求管道已断开。",
            };
        }

        std::vector<char> response_bytes;
        response_bytes.reserve(4096);
        std::vector<char> diagnostic_bytes;
        diagnostic_bytes.reserve(1024);
        bool response_limit = false;
        diagnostic_limit = false;
        const ULONGLONG deadline = GetTickCount64() + kOcrProcessTimeoutMs;
        std::optional<std::uint32_t> expected_response_size;
        for (;;) {
            if (stop_token.stop_requested()) {
                discard_locked(122, kWarmWorkerCancelWaitMs);
                return {WarmAttemptStatus::cancelled, {}, L"OCR 已取消。"};
            }
            const bool stdout_ok = append_available_output(
                stdout_read_.get(),
                response_bytes,
                kMaxWarmWorkerResponseBytes + 4U,
                response_limit);
            const bool stderr_ok = append_available_output(
                stderr_read_.get(),
                diagnostic_bytes,
                kMaxOcrProcessDiagnosticBytes,
                diagnostic_limit);
            if (!stdout_ok || !stderr_ok || response_limit || diagnostic_limit) {
                discard_locked(125);
                return {
                    WarmAttemptStatus::unavailable,
                    {},
                    response_limit
                        ? L"温热 OCR worker 响应超过 8 MiB。"
                        : diagnostic_limit
                              ? L"温热 OCR worker 诊断超过 1 MiB。"
                              : L"温热 OCR worker 管道读取失败。",
                };
            }
            if (!expected_response_size && response_bytes.size() >= 4U) {
                expected_response_size = decode_warm_frame_size(
                    std::span<const char>(response_bytes).first(4),
                    kMaxWarmWorkerResponseBytes);
                if (!expected_response_size) {
                    discard_locked(125);
                    return {
                        WarmAttemptStatus::unavailable,
                        {},
                        L"温热 OCR worker 帧长度无效。",
                    };
                }
            }
            if (expected_response_size) {
                const std::size_t frame_size = 4U + *expected_response_size;
                if (response_bytes.size() > frame_size) {
                    discard_locked(125);
                    return {
                        WarmAttemptStatus::unavailable,
                        {},
                        L"温热 OCR worker 返回了帧外数据。",
                    };
                }
                if (response_bytes.size() == frame_size) {
                    const std::string_view response(
                        response_bytes.data() + 4U,
                        *expected_response_size);
                    std::wstring response_error;
                    const auto parsed = parse_warm_worker_response(
                        response,
                        request_id,
                        profile,
                        dependency_lease.root(),
                        dependency_lease.sequence(),
                        image,
                        &response_error);
                    if (!parsed) {
                        discard_locked(125);
                        return {
                            WarmAttemptStatus::unavailable,
                            {},
                            L"温热 OCR worker 协议错误：" + response_error,
                        };
                    }
                    if (!parsed->ok) {
                        const std::wstring worker_error = parsed->error;
                        discard_locked(125);
                        return {
                            WarmAttemptStatus::unavailable,
                            {},
                            L"温热 OCR worker 执行失败：" + worker_error,
                        };
                    }
                    return {
                        WarmAttemptStatus::completed,
                        parsed->output,
                        {},
                    };
                }
            }

            const DWORD wait_result = WaitForSingleObject(process_.get(), 25);
            if (wait_result == WAIT_OBJECT_0) {
                discard_locked(125);
                return {
                    WarmAttemptStatus::unavailable,
                    {},
                    L"温热 OCR worker 在响应前退出。",
                };
            }
            if (wait_result == WAIT_FAILED) {
                discard_locked(125);
                return {
                    WarmAttemptStatus::unavailable,
                    {},
                    L"等待温热 OCR worker 时发生错误。",
                };
            }
            if (GetTickCount64() >= deadline) {
                discard_locked(124);
                return {
                    WarmAttemptStatus::unavailable,
                    {},
                    L"温热 OCR worker 超过 120 秒。",
                };
            }
        }
    }

    void stop() {
        std::scoped_lock lock(mutex_);
        discard_locked(125);
    }

private:
    WarmOcrWorkerManager() = default;

    static std::uint64_t next_request_id() noexcept {
        static std::atomic<std::uint64_t> next{1};
        std::uint64_t value = next.fetch_add(1, std::memory_order_relaxed);
        if (value == 0 || value > kMaxSafeJsonInteger) {
            next.store(2, std::memory_order_relaxed);
            value = 1;
        }
        return value;
    }

    bool worker_healthy_locked() {
        if (!process_.get()) {
            return false;
        }
        const DWORD wait_result = WaitForSingleObject(process_.get(), 0);
        if (wait_result == WAIT_TIMEOUT) {
            return true;
        }
        discard_locked(125);
        return false;
    }

    bool key_matches_locked(
        const std::filesystem::path& root,
        std::uint64_t sequence,
        std::wstring_view profile,
        int ort_threads) const {
        return warm_worker_key_matches_impl(
            process_.get() != nullptr,
            root_,
            sequence_,
            profile_,
            ort_threads_,
            root,
            sequence,
            profile,
            ort_threads);
    }

    bool write_all_locked(std::span<const std::byte> bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                bytes.size() - offset, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(
                    stdin_write_.get(),
                    bytes.data() + offset,
                    chunk,
                    &written,
                    nullptr) ||
                written == 0) {
                return false;
            }
            offset += written;
        }
        return true;
    }

    bool start_locked(
        const OcrDependencyLease& dependency_lease,
        std::wstring_view profile,
        const std::filesystem::path& model_directory,
        int ort_threads,
        std::wstring* error) {
        const std::filesystem::path runner_path =
            dependency_lease.root() / L"rapidocr_runner.exe";
        std::wstring runner_error;
        auto runner_guard = open_locked_path(runner_path, false, &runner_error);
        if (!runner_guard) {
            if (error) {
                *error = L"无法锁定温热 OCR runner：" + runner_error;
            }
            return false;
        }
        const auto verified_runner = final_path_from_handle(
            runner_guard->get(), &runner_error);
        if (!verified_runner) {
            if (error) {
                *error = L"无法解析温热 OCR runner 最终路径：" + runner_error;
            }
            return false;
        }

        std::vector<UniqueHandle> dependency_guards;
        dependency_guards.reserve(dependency_lease.handles().size());
        for (const HANDLE source : dependency_lease.handles()) {
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(
                    GetCurrentProcess(),
                    source,
                    GetCurrentProcess(),
                    &duplicate,
                    0,
                    FALSE,
                    DUPLICATE_SAME_ACCESS)) {
                if (error) {
                    *error = L"无法延长温热 OCR 依赖锁：" +
                             windows_error_message(GetLastError());
                }
                return false;
            }
            dependency_guards.emplace_back(duplicate);
        }

        SECURITY_ATTRIBUTES pipe_security{sizeof(pipe_security), nullptr, TRUE};
        HANDLE raw_stdin_read = nullptr;
        HANDLE raw_stdin_write = nullptr;
        if (!CreatePipe(
                &raw_stdin_read,
                &raw_stdin_write,
                &pipe_security,
                static_cast<DWORD>(kMaxWarmWorkerRequestBytes + 4U))) {
            if (error) {
                *error = L"创建温热 OCR 输入管道失败。";
            }
            return false;
        }
        UniqueHandle child_stdin(raw_stdin_read);
        UniqueHandle parent_stdin(raw_stdin_write);
        HANDLE raw_stdout_read = nullptr;
        HANDLE raw_stdout_write = nullptr;
        if (!CreatePipe(&raw_stdout_read, &raw_stdout_write, &pipe_security, 0)) {
            if (error) {
                *error = L"创建温热 OCR 输出管道失败。";
            }
            return false;
        }
        UniqueHandle parent_stdout(raw_stdout_read);
        UniqueHandle child_stdout(raw_stdout_write);
        HANDLE raw_stderr_read = nullptr;
        HANDLE raw_stderr_write = nullptr;
        if (!CreatePipe(&raw_stderr_read, &raw_stderr_write, &pipe_security, 0)) {
            if (error) {
                *error = L"创建温热 OCR 诊断管道失败。";
            }
            return false;
        }
        UniqueHandle parent_stderr(raw_stderr_read);
        UniqueHandle child_stderr(raw_stderr_write);
        if (!SetHandleInformation(parent_stdin.get(), HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(parent_stdout.get(), HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(parent_stderr.get(), HANDLE_FLAG_INHERIT, 0)) {
            if (error) {
                *error = L"限制温热 OCR 管道句柄继承失败。";
            }
            return false;
        }

        UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
        if (!job.get() || !configure_kill_on_close_job(job.get())) {
            if (error) {
                *error = L"创建温热 OCR Job 失败。";
            }
            return false;
        }

        SIZE_T attribute_bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_bytes);
        if (attribute_bytes == 0) {
            if (error) {
                *error = L"初始化温热 OCR 句柄白名单失败。";
            }
            return false;
        }
        std::vector<std::byte> attribute_storage(attribute_bytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            attribute_storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 2, 0, &attribute_bytes)) {
            if (error) {
                *error = L"初始化温热 OCR 句柄白名单失败。";
            }
            return false;
        }
        HANDLE inherited_handles[]{
            child_stdin.get(), child_stdout.get(), child_stderr.get()};
        if (!UpdateProcThreadAttribute(
                attributes,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited_handles,
                sizeof(inherited_handles),
                nullptr,
                nullptr)) {
            DeleteProcThreadAttributeList(attributes);
            if (error) {
                *error = L"配置温热 OCR 句柄白名单失败。";
            }
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
            if (error) {
                *error = L"配置温热 OCR 进程缓解策略失败。";
            }
            return false;
        }

        std::wstring command_line = quote_argument(verified_runner->wstring());
        command_line += L" --server --model-dir " + quote_argument(model_directory.wstring());
        command_line += L" --ocr-profile " + quote_argument(profile);
        command_line += L" --ort-threads " + std::to_wstring(std::clamp(ort_threads, 1, 4));
        command_line += L" --dependency-root " +
                        quote_argument(dependency_lease.root().wstring());
        command_line += L" --dependency-sequence " +
                        std::to_wstring(dependency_lease.sequence());

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_stdin.get();
        startup.StartupInfo.hStdOutput = child_stdout.get();
        startup.StartupInfo.hStdError = child_stderr.get();
        startup.lpAttributeList = attributes;
        auto environment = warm_runner_environment_block();
        const auto working_directory = warm_system_directory();
        const auto inherited_dll_directory = warm_current_dll_directory();
        if (!environment || working_directory.empty() || !inherited_dll_directory) {
            DeleteProcThreadAttributeList(attributes);
            if (error) {
                *error = L"无法构造温热 OCR runner 隔离环境。";
            }
            return false;
        }
        const bool clear_dll_directory = !inherited_dll_directory->empty();
        if (clear_dll_directory && !SetDllDirectoryW(L"")) {
            const DWORD dll_error = GetLastError();
            DeleteProcThreadAttributeList(attributes);
            if (error) {
                *error = L"无法隔离温热 OCR DLL 搜索目录：" +
                         windows_error_message(dll_error);
            }
            return false;
        }

        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            verified_runner->c_str(),
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
            !clear_dll_directory || SetDllDirectoryW(inherited_dll_directory->c_str());
        const DWORD restore_error = restored ? ERROR_SUCCESS : GetLastError();
        DeleteProcThreadAttributeList(attributes);
        UniqueHandle process_handle(process.hProcess);
        UniqueHandle thread_handle(process.hThread);
        child_stdin.reset();
        child_stdout.reset();
        child_stderr.reset();
        if (!restored) {
            if (created) {
                TerminateProcess(process_handle.get(), 125);
                WaitForSingleObject(process_handle.get(), kWarmWorkerCancelWaitMs);
            }
            if (error) {
                *error = L"无法恢复 OCR DLL 搜索目录：" +
                         windows_error_message(restore_error);
            }
            return false;
        }
        if (!created) {
            if (error) {
                *error = L"无法启动温热 OCR runner：" +
                         windows_error_message(create_error);
            }
            return false;
        }
        if (!AssignProcessToJobObject(job.get(), process_handle.get())) {
            TerminateProcess(process_handle.get(), 125);
            WaitForSingleObject(process_handle.get(), kWarmWorkerCancelWaitMs);
            if (error) {
                *error = L"无法隔离温热 OCR runner：" +
                         windows_error_message(GetLastError());
            }
            return false;
        }
        if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
            TerminateJobObject(job.get(), 125);
            WaitForSingleObject(process_handle.get(), kWarmWorkerCancelWaitMs);
            if (error) {
                *error = L"无法恢复温热 OCR runner 线程。";
            }
            return false;
        }

        root_ = dependency_lease.root();
        sequence_ = dependency_lease.sequence();
        profile_ = profile;
        ort_threads_ = std::clamp(ort_threads, 1, 4);
        dependency_guards_ = std::move(dependency_guards);
        runner_guard_ = std::move(*runner_guard);
        job_ = std::move(job);
        process_ = std::move(process_handle);
        stdin_write_ = std::move(parent_stdin);
        stdout_read_ = std::move(parent_stdout);
        stderr_read_ = std::move(parent_stderr);
        return true;
    }

    void discard_locked(DWORD exit_code, DWORD wait_ms = kWarmWorkerCancelWaitMs) {
        stdin_write_.reset();
        if (job_.get() && process_.get() &&
            WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) {
            TerminateJobObject(job_.get(), exit_code);
            WaitForSingleObject(process_.get(), wait_ms);
        }
        job_.reset();
        process_.reset();
        stdout_read_.reset();
        stderr_read_.reset();
        runner_guard_.reset();
        dependency_guards_.clear();
        root_.clear();
        sequence_ = 0;
        profile_.clear();
        ort_threads_ = 0;
    }

    std::mutex mutex_;
    std::filesystem::path root_;
    std::uint64_t sequence_{};
    std::wstring profile_;
    int ort_threads_{};
    std::vector<UniqueHandle> dependency_guards_;
    UniqueHandle runner_guard_;
    UniqueHandle job_;
    UniqueHandle process_;
    UniqueHandle stdin_write_;
    UniqueHandle stdout_read_;
    UniqueHandle stderr_read_;
};

void stop_warm_ocr_worker() {
    WarmOcrWorkerManager::instance().stop();
}

}  // namespace

namespace ocr_test_support {

std::optional<std::uint64_t>
parse_sequence(std::string_view value) {
    return parse_sequence_value(value);
}

bool sequence_is_allowed(
    std::uint64_t sequence,
    std::uint64_t persisted) noexcept {
    return sequence_is_allowed_by_policy(
        sequence,
        persisted);
}

std::optional<std::uint64_t>
read_sequence_file(
    const std::filesystem::path& path,
    bool allow_missing,
    std::wstring* error) {
    return read_sequence_file_locked(
        path,
        allow_missing,
        error);
}

bool update_sequence_file(
    const std::filesystem::path& path,
    std::uint64_t sequence,
    std::wstring* error) {
    if (sequence < 1 ||
        sequence > kMaxSafeJsonInteger) {
        if (error) {
            *error = L"OCR 序列号超出有效范围。";
        }
        return false;
    }
    const auto existing =
        read_sequence_file_locked(
            path,
            true,
            error);
    if (!existing) {
        return false;
    }
    if (sequence < *existing) {
        if (error) {
            *error = L"OCR 序列高水位不能回退。";
        }
        return false;
    }
    return sequence == *existing ||
           write_sequence_file_locked(
               path,
               sequence,
               error);
}

Bitmap resize_bitmap_high_quality(
    const Bitmap& source,
    int target_width,
    int target_height) {
    return resize_bitmap_high_quality_impl(
        source,
        target_width,
        target_height);
}

double select_preprocess_scale(
    int width,
    int height) noexcept {
    return select_preprocess_scale_impl(width, height);
}

int select_thread_count(
    std::uint64_t pixels,
    unsigned int logical_processors,
    bool accurate_profile) noexcept {
    return select_thread_count_impl(
        pixels,
        logical_processors,
        accurate_profile);
}

std::optional<WarmWorkerResponseResult>
parse_warm_worker_response(
    std::string_view response_utf8,
    std::uint64_t expected_request_id,
    std::wstring_view expected_profile,
    const std::filesystem::path& expected_root,
    std::uint64_t expected_sequence,
    const OcrProtocolExpectations& expected_image,
    std::wstring* protocol_error) {
    const auto parsed = ::airshot::parse_warm_worker_response(
        response_utf8,
        expected_request_id,
        expected_profile,
        expected_root,
        expected_sequence,
        expected_image,
        protocol_error);
    if (!parsed) {
        return std::nullopt;
    }
    return WarmWorkerResponseResult{
        parsed->ok,
        parsed->output,
        parsed->error,
    };
}

bool warm_worker_key_matches(
    bool process_healthy,
    const std::filesystem::path& current_root,
    std::uint64_t current_sequence,
    std::wstring_view current_profile,
    int current_ort_threads,
    const std::filesystem::path& requested_root,
    std::uint64_t requested_sequence,
    std::wstring_view requested_profile,
    int requested_ort_threads) {
    return warm_worker_key_matches_impl(
        process_healthy,
        current_root,
        current_sequence,
        current_profile,
        current_ort_threads,
        requested_root,
        requested_sequence,
        requested_profile,
        requested_ort_threads);
}

bool dependency_hash_cache_key_matches(
    bool same_file_object_value,
    const std::filesystem::path& cached_root,
    std::uint64_t cached_sequence,
    std::wstring_view cached_relative_path,
    std::wstring_view cached_sha256,
    std::uint64_t cached_size,
    const std::filesystem::path& requested_root,
    std::uint64_t requested_sequence,
    std::wstring_view requested_relative_path,
    std::wstring_view requested_sha256,
    std::uint64_t requested_size) {
    return dependency_hash_cache_key_matches_impl(
        same_file_object_value,
        cached_root,
        cached_sequence,
        cached_relative_path,
        cached_sha256,
        cached_size,
        requested_root,
        requested_sequence,
        requested_relative_path,
        requested_sha256,
        requested_size);
}

std::optional<std::wstring> sha256_file(
    const std::filesystem::path& path,
    std::stop_token stop_token,
    std::wstring* error) {
    if (ocr_stop_requested(stop_token, error)) {
        return std::nullopt;
    }
    auto handle = open_locked_path(
        path,
        false,
        error);
    if (!handle) {
        return std::nullopt;
    }
    const auto digest = sha256_handle(
        handle->get(),
        error,
        stop_token);
    if (!digest) {
        return std::nullopt;
    }
    return hex_digest(*digest);
}

bool warm_failure_allows_fallback(
    bool cancelled,
    bool stop_requested) noexcept {
    return warm_attempt_allows_fallback(
        cancelled ? WarmAttemptStatus::cancelled : WarmAttemptStatus::unavailable,
        stop_requested);
}

std::optional<std::uint32_t> decode_warm_frame_size(
    std::span<const char> header,
    std::size_t maximum) noexcept {
    return ::airshot::decode_warm_frame_size(header, maximum);
}

HANDLE lock_path(
    const std::filesystem::path& path,
    bool directory,
    std::wstring* error) {
    auto handle =
        open_locked_path(
            path,
            directory,
            error);
    return handle
               ? handle->release()
               : nullptr;
}

}  // namespace ocr_test_support

std::optional<OcrDependencyLease>
acquire_ocr_dependency_lease(
    const std::filesystem::path& root,
    bool verify_hashes,
    bool update_high_watermark,
    std::wstring* error,
    std::stop_token stop_token) {
    if (error) {
        error->clear();
    }
    try {
        if (stop_token.stop_requested()) {
            if (error) {
                *error = L"OCR 已取消。";
            }
            return std::nullopt;
        }
        const auto persisted =
            read_sequence_high_watermark(
                error,
                stop_token);
        if (!persisted) {
            return std::nullopt;
        }
        const std::uint64_t minimum =
            std::max(
                kCompiledMinimumSequence,
                *persisted);
        std::error_code path_error;
        const auto normalized_root =
            std::filesystem::absolute(
                root,
                path_error)
                .lexically_normal();
        if (path_error) {
            if (error) {
                *error =
                    L"无法解析 OCR 依赖根目录。";
            }
            return std::nullopt;
        }
        auto verified =
            verify_and_lock_installed_dependency(
                normalized_root,
                verify_hashes,
                minimum,
                stop_token,
                error);
        if (!verified) {
            return std::nullopt;
        }
        if (update_high_watermark &&
            !persist_sequence_high_watermark(
                verified->manifest.sequence,
                error,
                stop_token)) {
            return std::nullopt;
        }

        if (ocr_stop_requested(stop_token, error)) {
            return std::nullopt;
        }

        std::vector<HANDLE> handles;
        handles.reserve(
            verified->handles.size());
        for (auto& handle :
             verified->handles) {
            handles.push_back(handle.release());
        }
        return OcrDependencyLease(
            std::move(handles),
            verified->manifest.sequence,
            std::move(verified->root));
    } catch (const std::bad_alloc&) {
        if (error) {
            *error =
                L"OCR 依赖校验内存不足。";
        }
        return std::nullopt;
    } catch (const std::exception& exception) {
        if (error) {
            *error =
                L"OCR 依赖校验失败：" +
                from_utf8(exception.what());
        }
        return std::nullopt;
    }
}

std::filesystem::path rapid_ocr_dependency_directory() {
    return config_directory() / L"ocr" / kRapidOcrOnnxPackageId;
}

std::optional<OcrDependencyManifest> parse_ocr_dependency_manifest(
    std::wstring_view json) {
    try {
        const ScopedWinrtApartment apartment;
        const auto root = winrt::Windows::Data::Json::JsonObject::Parse(json);
        if (root.Size() != 6 || !root.HasKey(L"schemaVersion") ||
            !root.HasKey(L"packageId") || !root.HasKey(L"sequence") ||
            !root.HasKey(L"issuedAt") || !root.HasKey(L"expiresAt") ||
            !root.HasKey(L"files")) {
            return std::nullopt;
        }

        const auto exact_integer = [&root](
                                       std::wstring_view name,
                                       std::uint64_t minimum,
                                       std::uint64_t maximum)
            -> std::optional<std::uint64_t> {
            const double value =
                root.GetNamedNumber(winrt::hstring(name));
            if (!std::isfinite(value) ||
                value < static_cast<double>(minimum) ||
                value > static_cast<double>(maximum) ||
                std::trunc(value) != value) {
                return std::nullopt;
            }
            return static_cast<std::uint64_t>(value);
        };

        const auto schema_version =
            exact_integer(L"schemaVersion", 1, kManifestSchemaVersion);
        const auto sequence =
            exact_integer(L"sequence", 1, kMaxSafeJsonInteger);
        const auto issued_at = exact_integer(
            L"issuedAt",
            kMinimumManifestTimestamp,
            kMaximumManifestTimestamp);
        const auto expires_at = exact_integer(
            L"expiresAt",
            kMinimumManifestTimestamp,
            kMaximumManifestTimestamp);
        if (!schema_version || *schema_version != kManifestSchemaVersion ||
            !sequence || !issued_at || !expires_at ||
            *expires_at <= *issued_at ||
            *expires_at - *issued_at > kMaximumManifestLifetimeSeconds) {
            return std::nullopt;
        }

        OcrDependencyManifest manifest;
        manifest.package_id = root.GetNamedString(L"packageId").c_str();
        if (manifest.package_id != kRapidOcrOnnxPackageId) {
            return std::nullopt;
        }
        manifest.sequence = *sequence;
        manifest.issued_at = *issued_at;
        manifest.expires_at = *expires_at;

        const auto files = root.GetNamedArray(L"files");
        if (files.Size() == 0 || files.Size() > kMaxManifestFiles) {
            return std::nullopt;
        }

        std::set<std::wstring> unique_paths;
        std::uint64_t total_size = 0;
        manifest.files.reserve(files.Size());
        for (const auto& value : files) {
            const auto object = value.GetObject();
            if (object.Size() != 4 || !object.HasKey(L"path") ||
                !object.HasKey(L"url") || !object.HasKey(L"sha256") ||
                !object.HasKey(L"size")) {
                return std::nullopt;
            }

            OcrDependencyFile file;
            const auto normalized_path =
                normalize_manifest_relative_path(object.GetNamedString(L"path").c_str());
            if (!normalized_path) {
                return std::nullopt;
            }
            file.path = *normalized_path;
            file.url = object.GetNamedString(L"url").c_str();
            file.sha256 =
                normalized_hex(object.GetNamedString(L"sha256").c_str());

            const double numeric_size = object.GetNamedNumber(L"size");
            if (!std::isfinite(numeric_size) || numeric_size < 1.0 ||
                numeric_size > static_cast<double>(kMaxDependencyFileBytes) ||
                std::trunc(numeric_size) != numeric_size) {
                return std::nullopt;
            }
            file.size = static_cast<std::uint64_t>(numeric_size);

            const std::wstring comparison_key = path_comparison_key(file.path);
            if (!unique_paths.insert(comparison_key).second ||
                !valid_https_url(file.url) || file.sha256.size() != 64 ||
                is_zero_hash(file.sha256) ||
                total_size > kMaxDependencyPackageBytes - file.size) {
                return std::nullopt;
            }
            total_size += file.size;
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

std::optional<OcrManifestSignature> parse_ocr_manifest_signature(
    std::wstring_view json) {
    try {
        const ScopedWinrtApartment apartment;
        const auto root = winrt::Windows::Data::Json::JsonObject::Parse(json);
        if (root.Size() != 2 || !root.HasKey(L"keyId") ||
            !root.HasKey(L"signature")) {
            return std::nullopt;
        }

        OcrManifestSignature result;
        result.key_id = root.GetNamedString(L"keyId").c_str();
        if (result.key_id.empty() || result.key_id.size() > 64 ||
            !std::ranges::all_of(result.key_id, [](wchar_t character) {
                return (character >= L'a' && character <= L'z') ||
                       (character >= L'A' && character <= L'Z') ||
                       (character >= L'0' && character <= L'9') ||
                       character == L'.' || character == L'_' || character == L'-';
            })) {
            return std::nullopt;
        }

        const auto signature =
            decode_hex(root.GetNamedString(L"signature").c_str());
        if (!signature || signature->size() != result.value.size()) {
            return std::nullopt;
        }
        std::ranges::copy(*signature, result.value.begin());
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

bool verify_ocr_manifest_signature(
    std::span<const std::byte> manifest_utf8,
    std::span<const std::uint8_t> signature,
    std::span<const std::uint8_t> public_key_xy,
    std::wstring* error) {
    if (manifest_utf8.empty() || manifest_utf8.size() > kMaxManifestBytes ||
        signature.size() != 64 || public_key_xy.size() != 64) {
        if (error) {
            *error = L"OCR 清单签名参数长度无效。";
        }
        return false;
    }

    const auto digest = sha256_bytes(manifest_utf8);
    if (!digest) {
        if (error) {
            *error = L"无法计算 OCR 清单 SHA256。";
        }
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_ECDSA_P256_ALGORITHM,
            nullptr,
            0))) {
        if (error) {
            *error = L"无法初始化 OCR ECDSA P-256 验证器。";
        }
        return false;
    }

    struct PublicKeyBlob {
        BCRYPT_ECCKEY_BLOB header;
        std::array<std::uint8_t, 64> coordinates;
    } blob{};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = 32;
    std::ranges::copy(public_key_xy, blob.coordinates.begin());

    NTSTATUS status = BCryptImportKeyPair(
        algorithm,
        nullptr,
        BCRYPT_ECCPUBLIC_BLOB,
        &key,
        reinterpret_cast<PUCHAR>(&blob),
        sizeof(blob),
        0);
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptVerifySignature(
            key,
            nullptr,
            const_cast<PUCHAR>(digest->data()),
            static_cast<ULONG>(digest->size()),
            const_cast<PUCHAR>(signature.data()),
            static_cast<ULONG>(signature.size()),
            0);
    }

    if (key) {
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);

    if (!BCRYPT_SUCCESS(status)) {
        if (error) {
            *error = L"OCR 依赖清单 ECDSA P-256 签名无效。";
        }
        return false;
    }
    return true;
}

int run_ocr_manifest_verifier(
    std::span<const std::wstring> arguments) {
    if (arguments.size() != 3 ||
        arguments[0] != L"--verify-ocr-manifest") {
        return static_cast<int>(ExitCode::invalid_arguments);
    }

    std::wstring error;
    const auto manifest_bytes = read_binary_file(
        std::filesystem::path(arguments[1]),
        kMaxManifestBytes,
        &error);
    const auto signature_bytes = read_binary_file(
        std::filesystem::path(arguments[2]),
        64U * 1024U,
        &error);
    if (!manifest_bytes || !signature_bytes ||
        !validate_signed_manifest(
            *manifest_bytes,
            *signature_bytes,
            true,
            nullptr,
            &error)) {
        return static_cast<int>(ExitCode::operation_failed);
    }
    return static_cast<int>(ExitCode::success);
}

OcrDependencyStatus ocr_dependency_status(std::wstring_view engine) {
    const auto& spec = ocr_engine_spec(engine);
    std::wstring first_problem;
    bool saw_partial_directory = false;
    for (const auto& root : ocr_dependency_roots()) {
        std::wstring problem;
        if (dependency_directory_ready(root, spec, &problem)) {
            return {
                true,
                false,
                L"状态: " + std::wstring(spec.label) + L" 就绪",
            };
        }
        if (!saw_partial_directory &&
            GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) {
            saw_partial_directory = true;
            first_problem = problem;
        }
    }
    if (saw_partial_directory) {
        return {
            false,
            false,
            L"状态: 现有 OCR 依赖不可信，需先人工移除：" +
                (first_problem.empty()
                     ? std::wstring(L"安全校验失败")
                     : first_problem),
        };
    }
    if (configured_manifest_key_id().empty() ||
        !configured_manifest_public_key()) {
        return {
            false,
            false,
            L"状态: 此构建未配置 OCR 清单公钥",
        };
    }
    return {false, true, L"状态: 未下载 RapidOCR ONNX 依赖"};
}

bool download_ocr_dependencies(
    std::wstring_view manifest_url,
    const std::function<void(int)>& progress_callback,
    std::wstring* error,
    std::stop_token stop_token) try {
    stop_warm_ocr_worker();
    clear_dependency_hash_cache();
    if (!valid_https_url(manifest_url) ||
        manifest_url.find_first_of(L"?#") != std::wstring_view::npos) {
        if (error) {
            *error = L"OCR 依赖清单地址必须是无 query/fragment 的 HTTPS URL。";
        }
        return false;
    }
    if (stop_token.stop_requested()) {
        if (error) {
            *error = L"OCR 依赖下载已取消。";
        }
        return false;
    }
    if (configured_manifest_key_id().empty() ||
        !configured_manifest_public_key()) {
        if (error) {
            *error = L"此构建未配置 OCR 清单生产公钥，已拒绝网络安装。";
        }
        return false;
    }

    auto install_mutex =
        acquire_named_mutex(
            kInstallMutexName,
            error);
    if (!install_mutex) {
        return false;
    }

    WinHttpDownloadContext download_context;
    if (!download_context.initialize(error)) {
        return false;
    }

    const ULONGLONG operation_deadline =
        GetTickCount64() + kDownloadOperationDeadlineMs;
    if (progress_callback) {
        progress_callback(0);
    }

    const auto root = config_directory() / L"ocr";
    const auto working_directory =
        create_private_directory(root, L".download-", error);
    if (!working_directory) {
        return false;
    }
    ScopedDirectoryCleanup cleanup(*working_directory);

    const auto manifest_path = *working_directory / L"manifest.json";
    const auto signature_path = *working_directory / L"manifest.json.sig";
    std::wstring download_error;
    if (!download_file(
            download_context,
            manifest_url,
            manifest_path,
            kMaxManifestBytes,
            operation_deadline,
            stop_token,
            &download_error) ||
        !download_file(
            download_context,
            std::wstring(manifest_url) + L".sig",
            signature_path,
            64U * 1024U,
            operation_deadline,
            stop_token,
            &download_error)) {
        if (error) {
            *error = L"下载 OCR 依赖清单或签名失败：" + download_error;
        }
        return false;
    }
    if (progress_callback) {
        progress_callback(5);
    }

    const auto manifest_bytes =
        read_binary_file(manifest_path, kMaxManifestBytes, error);
    const auto signature_bytes =
        read_binary_file(signature_path, 64U * 1024U, error);
    if (!manifest_bytes || !signature_bytes) {
        return false;
    }

    OcrDependencyManifest manifest;
    if (!validate_signed_manifest(
            *manifest_bytes,
            *signature_bytes,
            true,
            &manifest,
            error)) {
        return false;
    }

    const auto persisted_sequence =
        read_sequence_high_watermark(error);
    if (!persisted_sequence) {
        return false;
    }
    std::uint64_t minimum_sequence =
        std::max(
            kCompiledMinimumSequence,
            *persisted_sequence);
    if (!sequence_is_allowed_by_policy(
            manifest.sequence,
            minimum_sequence)) {
        if (error) {
            *error = std::format(
                L"已拒绝 OCR 依赖回滚：最低允许序列为 {}，下载序列为 {}。",
                minimum_sequence,
                manifest.sequence);
        }
        return false;
    }

    for (const auto& existing_root : ocr_dependency_roots()) {
        if (GetFileAttributesW(existing_root.c_str()) ==
            INVALID_FILE_ATTRIBUTES) {
            continue;
        }

        std::wstring existing_error;
        auto existing_lease =
            acquire_ocr_dependency_lease(
                existing_root,
                false,
                false,
                &existing_error);
        if (!existing_lease) {
            if (error) {
                *error =
                    L"现有 OCR 依赖目录不可信，已拒绝覆盖：" +
                    existing_root.wstring() + L"：" +
                    (existing_error.empty()
                         ? std::wstring(L"安全校验失败")
                         : existing_error);
            }
            return false;
        }
        minimum_sequence =
            std::max(
                minimum_sequence,
                existing_lease->sequence());

        const auto existing_manifest_path =
            existing_root / kInstalledManifestName;
        const auto existing_manifest_bytes = read_binary_file(
            existing_manifest_path,
            kMaxManifestBytes,
            &existing_error);
        if (!existing_manifest_bytes) {
            if (error) {
                *error =
                    L"无法在锁定状态下读取现有 OCR 清单：" +
                    existing_error;
            }
            return false;
        }
        if (manifest.sequence <
            existing_lease->sequence()) {
            if (error) {
                *error = std::format(
                    L"已拒绝 OCR 依赖回滚：已安装序列 {}，下载序列 {}。",
                    existing_lease->sequence(),
                    manifest.sequence);
            }
            return false;
        }
        if (manifest.sequence ==
                existing_lease->sequence() &&
            !std::ranges::equal(
                *manifest_bytes,
                *existing_manifest_bytes)) {
            if (error) {
                *error = std::format(
                    L"OCR 依赖序列 {} 已安装，但签名清单内容不同。",
                    manifest.sequence);
            }
            return false;
        }
    }
    if (!sequence_is_allowed_by_policy(
            manifest.sequence,
            minimum_sequence)) {
        if (error) {
            *error = std::format(
                L"已拒绝 OCR 依赖回滚：有效安装的最高序列为 {}，下载序列为 {}。",
                minimum_sequence,
                manifest.sequence);
        }
        return false;
    }

    const auto staging_directory = *working_directory / L"payload";
    std::error_code filesystem_error;
    std::filesystem::create_directory(staging_directory, filesystem_error);
    if (filesystem_error ||
        !is_directory_without_reparse(staging_directory)) {
        if (error) {
            *error = L"无法创建 OCR 依赖 staging 目录。";
        }
        return false;
    }

    for (std::size_t i = 0; i < manifest.files.size(); ++i) {
        if (stop_token.stop_requested()) {
            if (error) {
                *error = L"OCR 依赖下载已取消。";
            }
            return false;
        }
        if (GetTickCount64() >= operation_deadline) {
            if (error) {
                *error = L"OCR 依赖下载超过 30 分钟总时间预算。";
            }
            return false;
        }
        const auto& file = manifest.files[i];
        const std::filesystem::path relative(file.path);
        const auto target = staging_directory / relative;
        std::filesystem::create_directories(target.parent_path(), filesystem_error);
        if (filesystem_error ||
            !safe_existing_path_components(
                staging_directory,
                relative.parent_path(),
                true)) {
            if (error) {
                *error = L"OCR 清单路径无法安全创建：" + file.path;
            }
            return false;
        }

        const auto temporary =
            target.parent_path() / (target.filename().wstring() + L".download");
        if (!download_file(
                download_context,
                file.url,
                temporary,
                file.size,
                operation_deadline,
                stop_token,
                error) ||
            !verify_dependency_file(file, temporary, error)) {
            return false;
        }
        if (!MoveFileExW(
                temporary.c_str(),
                target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ||
            !is_regular_non_reparse_file(target)) {
            if (error) {
                *error =
                    L"无法安全写入 OCR 依赖文件：" +
                    windows_error_message(GetLastError());
            }
            return false;
        }

        if (progress_callback) {
            const int progress =
                5 + static_cast<int>(((i + 1) * 90) / manifest.files.size());
            progress_callback(std::clamp(progress, 5, 95));
        }
    }

    if (stop_token.stop_requested()) {
        if (error) {
            *error = L"OCR 依赖下载已取消。";
        }
        return false;
    }
    if (!write_binary_file(
            staging_directory / kInstalledManifestName,
            *manifest_bytes,
            error) ||
        !write_binary_file(
            staging_directory / kInstalledSignatureName,
            *signature_bytes,
            error)) {
        return false;
    }

    const auto final_directory = rapid_ocr_dependency_directory();
    const auto backup_directory = *working_directory / L"previous";
    const auto latest_persisted =
        read_sequence_high_watermark(error);
    if (!latest_persisted) {
        return false;
    }
    const std::uint64_t latest_minimum =
        std::max(
            kCompiledMinimumSequence,
            *latest_persisted);
    if (!sequence_is_allowed_by_policy(
            manifest.sequence,
            latest_minimum)) {
        if (error) {
            *error = std::format(
                L"安装前检测到更高 OCR 序列 {}，已拒绝候选序列 {}。",
                latest_minimum,
                manifest.sequence);
        }
        return false;
    }

    const bool had_previous =
        GetFileAttributesW(final_directory.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (had_previous &&
        !MoveFileExW(
            final_directory.c_str(),
            backup_directory.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
        if (error) {
            *error =
                L"无法备份现有 OCR 依赖：" +
                windows_error_message(GetLastError());
        }
        return false;
    }

    if (!MoveFileExW(
            staging_directory.c_str(),
            final_directory.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = GetLastError();
        const bool restored =
            !had_previous ||
            MoveFileExW(
                backup_directory.c_str(),
                final_directory.c_str(),
                MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!restored) {
            cleanup.release();
        }
        if (error) {
            *error =
                L"无法安装 OCR 依赖：" + windows_error_message(move_error);
            if (!restored) {
                *error +=
                    L"；恢复旧版本也失败，备份已保留在：" +
                    backup_directory.wstring();
            }
        }
        return false;
    }

    const auto failed_directory =
        *working_directory / L"failed-installed";
    auto rollback_installed_directory =
        [&](std::wstring reason) {
            const bool quarantined =
                MoveFileExW(
                    final_directory.c_str(),
                    failed_directory.c_str(),
                    MOVEFILE_WRITE_THROUGH) != FALSE;
            const DWORD quarantine_error =
                quarantined
                    ? ERROR_SUCCESS
                    : GetLastError();
            const bool restored =
                quarantined &&
                (!had_previous ||
                 MoveFileExW(
                     backup_directory.c_str(),
                     final_directory.c_str(),
                     MOVEFILE_WRITE_THROUGH) != FALSE);
            const DWORD restore_error =
                restored
                    ? ERROR_SUCCESS
                    : GetLastError();
            if (!restored) {
                cleanup.release();
            }
            if (error) {
                *error = std::move(reason);
                if (!quarantined) {
                    *error +=
                        L"；无法隔离失败安装：" +
                        windows_error_message(
                            quarantine_error);
                } else if (!restored) {
                    *error +=
                        L"；无法恢复旧版本：" +
                        windows_error_message(
                            restore_error);
                }
                if (!restored) {
                    *error +=
                        L"；恢复材料已保留在：" +
                        working_directory->wstring();
                }
            }
            return false;
        };

    std::wstring final_verification_error;
    auto final_lease =
        acquire_ocr_dependency_lease(
            final_directory,
            true,
            false,
            &final_verification_error);
    if (!final_lease ||
        final_lease->sequence() !=
            manifest.sequence) {
        final_lease.reset();
        return rollback_installed_directory(
            L"OCR 依赖完成移动后复验失败：" +
            (final_verification_error.empty()
                 ? std::wstring(
                       L"序列号与下载清单不一致")
                 : final_verification_error));
    }
    if (!persist_sequence_high_watermark(
            manifest.sequence,
            &final_verification_error)) {
        final_lease.reset();
        return rollback_installed_directory(
            L"OCR 依赖已复验，但无法更新单调序列状态：" +
            final_verification_error);
    }

    if (progress_callback) {
        progress_callback(100);
    }
    return true;
} catch (const std::bad_alloc&) {
    if (error) {
        *error =
            L"OCR 依赖下载内存不足。";
    }
    return false;
} catch (const std::exception& exception) {
    if (error) {
        *error =
            L"OCR 依赖下载异常：" +
            from_utf8(exception.what());
    }
    return false;
} catch (...) {
    if (error) {
        *error =
            L"OCR 依赖下载发生未知异常。";
    }
    return false;
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

OcrOutput recognize_text(
    const Bitmap& bitmap,
    const AppConfig& config,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        return {false, {}, L"OCR 已取消。"};
    }
    if (bitmap.empty()) {
        return {false, {}, L"OCR 图像为空。"};
    }

    const auto& spec = ocr_engine_spec(config.ocr_engine);
    std::wstring dependency_problem;
    std::optional<std::filesystem::path>
        runtime_directory;
    std::optional<OcrDependencyLease>
        dependency_lease;
    for (const auto& root :
         ocr_dependency_roots()) {
        if (stop_token.stop_requested()) {
            return {false, {}, L"OCR 已取消。"};
        }
        std::wstring candidate_problem;
        auto candidate =
            acquire_ocr_dependency_lease(
                root,
                true,
                true,
                &candidate_problem,
                stop_token);
        if (candidate) {
            runtime_directory =
                candidate->root();
            dependency_lease =
                std::move(*candidate);
            break;
        }
        if (stop_token.stop_requested()) {
            return {false, {}, L"OCR 已取消。"};
        }
        if (dependency_problem.empty() &&
            GetFileAttributesW(root.c_str()) !=
                INVALID_FILE_ATTRIBUTES) {
            dependency_problem =
                std::move(candidate_problem);
        }
    }
    if (!runtime_directory) {
        return {
            false,
            {},
            dependency_problem.empty()
                ? L"未找到 OCR 依赖，请在设置中点击“下载依赖”。"
                : L"OCR 依赖校验失败：" + dependency_problem +
                      L"。请在设置中重新下载依赖。",
        };
    }

    if (stop_token.stop_requested()) {
        return {false, {}, L"OCR 已取消。"};
    }
    std::wstring temporary_error;
    const auto temporary_directory = create_private_directory(
        config_directory(),
        L".ocr-",
        &temporary_error);
    if (!temporary_directory) {
        return {
            false,
            {},
            L"无法创建私有 OCR 临时目录：" + temporary_error,
        };
    }
    ScopedDirectoryCleanup cleanup(*temporary_directory);

    Bitmap resized_bitmap;
    const Bitmap* ocr_bitmap = &bitmap;
    std::wstring preprocess_mode = L"none";
    try {
        const double requested_scale =
            select_preprocess_scale_impl(bitmap.width, bitmap.height);
        const int target_width = std::clamp(
            static_cast<int>(std::lround(bitmap.width * requested_scale)),
            1,
            kMaxOcrImageEdge);
        const int target_height = std::clamp(
            static_cast<int>(std::lround(bitmap.height * requested_scale)),
            1,
            kMaxOcrImageEdge);
        if (target_width != bitmap.width || target_height != bitmap.height) {
            resized_bitmap = resize_bitmap_high_quality_impl(
                bitmap,
                target_width,
                target_height);
            ocr_bitmap = &resized_bitmap;
            preprocess_mode = requested_scale > 1.0
                                  ? L"bilinear-upscale"
                                  : L"progressive-bilinear-downscale";
        }
    } catch (const std::bad_alloc&) {
        return {
            false,
            {},
            L"OCR 图像缩放内存分配失败。",
        };
    } catch (const std::length_error&) {
        return {
            false,
            {},
            L"OCR 图像尺寸超出可处理范围。",
        };
    }
    if (ocr_bitmap->empty()) {
        return {
            false,
            {},
            L"OCR 图像缩放内存分配失败。",
        };
    }
    if (stop_token.stop_requested()) {
        return {false, {}, L"OCR 已取消。"};
    }
    const std::filesystem::path temporary_png =
        *temporary_directory / L"selection.png";
    std::wstring save_error;
    if (!save_png(*ocr_bitmap, temporary_png, &save_error)) {
        return {
            false,
            {},
            L"无法保存临时 OCR 选区图像：" + save_error,
        };
    }
    SetFileAttributesW(
        temporary_png.c_str(),
        FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_TEMPORARY);

    if (stop_token.stop_requested()) {
        return {false, {}, L"OCR 已取消。"};
    }
    const std::filesystem::path executable = portable_executable_path();
    if (!is_regular_non_reparse_file(executable)) {
        return {
            false,
            {},
            L"未找到可信的 " + executable.filename().wstring() +
                L"，请重新编译或安装程序。",
        };
    }

    std::wstring command_line = quote_argument(executable.wstring());
    command_line +=
        L" --ocr-internal --engine onnx --image " +
        quote_argument(temporary_png.wstring());
    command_line +=
        L" --model-dir " +
        quote_argument(
            (*runtime_directory /
             std::wstring(spec.profile_directory))
                .wstring());
    command_line +=
        L" --dependency-dir " +
        quote_argument(runtime_directory->wstring());
    command_line +=
        L" --ocr-profile " + quote_argument(std::wstring(spec.id));
    const double scale_x =
        static_cast<double>(ocr_bitmap->width) / static_cast<double>(bitmap.width);
    const double scale_y =
        static_cast<double>(ocr_bitmap->height) / static_cast<double>(bitmap.height);
    const std::uint64_t input_pixels =
        static_cast<std::uint64_t>(ocr_bitmap->width) *
        static_cast<std::uint64_t>(ocr_bitmap->height);
    unsigned int processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (processors == 0) {
        processors = 1;
    }
    const int ort_threads = select_thread_count_impl(
        input_pixels,
        processors,
        spec.id == kOcrEngineRapidV5Accurate);
    command_line += L" --source-width " + std::to_wstring(bitmap.width);
    command_line += L" --source-height " + std::to_wstring(bitmap.height);
    command_line += L" --input-width " + std::to_wstring(ocr_bitmap->width);
    command_line += L" --input-height " + std::to_wstring(ocr_bitmap->height);
    command_line += L" --scale-x " + std::format(L"{:.17g}", scale_x);
    command_line += L" --scale-y " + std::format(L"{:.17g}", scale_y);
    command_line += L" --preprocess-mode " + quote_argument(preprocess_mode);
    command_line += L" --ort-threads " + std::to_wstring(ort_threads);

    const OcrProtocolExpectations expected{
        spec.id,
        bitmap.width,
        bitmap.height,
        ocr_bitmap->width,
        ocr_bitmap->height,
        scale_x,
        scale_y,
        preprocess_mode,
    };

    WarmAttempt warm_attempt = WarmOcrWorkerManager::instance().recognize(
        *dependency_lease,
        spec.id,
        *runtime_directory / std::wstring(spec.profile_directory),
        temporary_png,
        expected,
        ort_threads,
        stop_token);
    if (warm_attempt.status == WarmAttemptStatus::completed) {
        return std::move(warm_attempt.output);
    }
    if (!warm_attempt_allows_fallback(
            warm_attempt.status,
            stop_token.stop_requested())) {
        return {false, {}, L"OCR 已取消。"};
    }

    return run_ocr_process(
        executable,
        std::move(command_line),
        *dependency_lease,
        expected,
        stop_token);
}

}  // namespace airshot
