#include "airshot/ocr.h"

#include "airshot/config.h"
#include "airshot/output.h"
#include "airshot/portable.h"

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
constexpr int kMaxOcrImageEdge = 4096;
constexpr std::uint64_t kMaxOcrPixels = 8ULL * 1024ULL * 1024ULL;
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

std::optional<MutexLease> acquire_named_mutex(
    const wchar_t* name,
    std::wstring* error) {
    UniqueHandle mutex(CreateMutexW(nullptr, FALSE, name));
    if (!mutex.get()) {
        if (error) {
            *error =
                L"无法创建 OCR 状态互斥锁：" +
                windows_error_message(GetLastError());
        }
        return std::nullopt;
    }

    const DWORD wait_result =
        WaitForSingleObject(mutex.get(), kOcrMutexWaitMs);
    if (wait_result != WAIT_OBJECT_0 &&
        wait_result != WAIT_ABANDONED) {
        if (error) {
            *error =
                wait_result == WAIT_TIMEOUT
                    ? L"等待 OCR 状态互斥锁超时。"
                    : L"等待 OCR 状态互斥锁失败：" +
                          windows_error_message(GetLastError());
        }
        return std::nullopt;
    }
    return MutexLease(mutex.release());
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
    std::wstring* error) {
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

    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while (BCRYPT_SUCCESS(status)) {
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
        status = BCryptHashData(
            hash,
            buffer.data(),
            bytes_read,
            0);
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
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
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
    std::wstring* error) {
    auto mutex = acquire_named_mutex(
        kSequenceMutexName,
        error);
    if (!mutex) {
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
    std::wstring* error) {
    if (sequence < 1 ||
        sequence > kMaxSafeJsonInteger) {
        if (error) {
            *error = L"OCR 序列号超出有效范围。";
        }
        return false;
    }

    auto mutex = acquire_named_mutex(
        kSequenceMutexName,
        error);
    if (!mutex) {
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

bool verify_dependency_handle(
    const OcrDependencyFile& file,
    HANDLE handle,
    bool verify_hash,
    std::wstring* error) {
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
        sha256_handle(handle, error);
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
    std::wstring* error) {
    VerifiedDependency result;
    result.handles.reserve(
        kMaxManifestFiles +
        kMaxDependencyDirectories + 8);

    std::vector<std::filesystem::path>
        protected_directories;
    std::filesystem::path current = root;
    for (int depth = 0; depth < 3; ++depth) {
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
        auto file_handle = open_locked_path(
            result.root /
                std::filesystem::path(entry.path),
            false,
            error);
        if (!file_handle ||
            !verify_dependency_handle(
                entry,
                file_handle->get(),
                verify_hashes,
                error)) {
            return std::nullopt;
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

Bitmap resize_bitmap_for_ocr(const Bitmap& source) {
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(std::max(0, source.width)) *
        static_cast<std::uint64_t>(std::max(0, source.height));
    if (source.empty() ||
        (source.width <= kMaxOcrImageEdge && source.height <= kMaxOcrImageEdge &&
         pixels <= kMaxOcrPixels)) {
        return source;
    }

    const double edge_scale = std::min(
        static_cast<double>(kMaxOcrImageEdge) / source.width,
        static_cast<double>(kMaxOcrImageEdge) / source.height);
    const double pixel_scale =
        std::sqrt(static_cast<double>(kMaxOcrPixels) / static_cast<double>(pixels));
    const double scale = std::min(edge_scale, pixel_scale);
    const int target_width =
        std::max(1, static_cast<int>(std::floor(source.width * scale)));
    const int target_height =
        std::max(1, static_cast<int>(std::floor(source.height * scale)));

    Bitmap target(target_width, target_height);
    if (target.empty()) {
        return {};
    }
    for (int y = 0; y < target_height; ++y) {
        const int source_y = std::min(
            source.height - 1,
            static_cast<int>(static_cast<double>(y) / scale));
        const auto* source_row = source.row(source_y).data();
        auto* target_row = target.row(y).data();
        for (int x = 0; x < target_width; ++x) {
            const int source_x = std::min(
                source.width - 1,
                static_cast<int>(static_cast<double>(x) / scale));
            std::memcpy(target_row + x * 4, source_row + source_x * 4, 4);
        }
    }
    return target;
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
        if (output.size() > kMaxOcrProcessOutputBytes - bytes_read) {
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
    const OcrDependencyLease& dependency_lease) {
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

    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    SECURITY_ATTRIBUTES pipe_security{sizeof(pipe_security), nullptr, TRUE};
    if (!CreatePipe(&raw_read, &raw_write, &pipe_security, 0)) {
        return {false, {}, L"创建 OCR 输出管道失败。"};
    }
    UniqueHandle stdout_read(raw_read);
    UniqueHandle stdout_write(raw_write);
    if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0)) {
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
    startup.StartupInfo.hStdError = stdout_write.get();
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

    std::vector<char> output;
    output.reserve(4096);
    const ULONGLONG deadline = GetTickCount64() + kOcrProcessTimeoutMs;
    bool timed_out = false;
    bool output_limit_exceeded = false;
    bool wait_failed = false;
    for (;;) {
        if (!append_available_output(
                stdout_read.get(),
                output,
                output_limit_exceeded)) {
            if (output_limit_exceeded) {
                TerminateJobObject(job.get(), 125);
                WaitForSingleObject(process_handle.get(), 5'000);
            }
            break;
        }

        const DWORD wait_result = WaitForSingleObject(process_handle.get(), 25);
        if (wait_result == WAIT_OBJECT_0) {
            append_available_output(
                stdout_read.get(),
                output,
                output_limit_exceeded);
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
        return {
            false,
            {},
            L"OCR 子进程超过 120 秒，已停止本次识别。",
        };
    }
    if (output_limit_exceeded) {
        return {
            false,
            {},
            L"OCR 子进程输出超过 8 MiB，已停止本次识别。",
        };
    }
    if (wait_failed) {
        return {false, {}, L"等待 OCR 子进程时发生错误。"};
    }

    const std::string output_bytes(output.begin(), output.end());
    const std::wstring decoded = from_utf8(output_bytes);
    if (!output.empty() && decoded.empty()) {
        return {false, {}, L"OCR 子进程返回了无效的 UTF-8 输出。"};
    }
    if (exit_code != 0) {
        return {
            false,
            {},
            decoded.empty() ? L"OCR 子进程执行失败。" : decoded,
        };
    }
    if (decoded.empty()) {
        return {false, {}, L"OCR 未识别到任何文本。"};
    }
    return {true, decoded, {}};
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
    std::wstring* error) {
    if (error) {
        error->clear();
    }
    try {
        const auto persisted =
            read_sequence_high_watermark(error);
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
                error);
        if (!verified) {
            return std::nullopt;
        }
        if (update_high_watermark &&
            !persist_sequence_high_watermark(
                verified->manifest.sequence,
                error)) {
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

OcrOutput recognize_text(const Bitmap& bitmap, const AppConfig& config) {
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
        std::wstring candidate_problem;
        auto candidate =
            acquire_ocr_dependency_lease(
                root,
                true,
                true,
                &candidate_problem);
        if (candidate) {
            runtime_directory =
                candidate->root();
            dependency_lease =
                std::move(*candidate);
            break;
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

    Bitmap ocr_bitmap;
    try {
        ocr_bitmap = resize_bitmap_for_ocr(bitmap);
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
    if (ocr_bitmap.empty()) {
        return {
            false,
            {},
            L"OCR 图像缩放内存分配失败。",
        };
    }
    const std::filesystem::path temporary_png =
        *temporary_directory / L"selection.png";
    std::wstring save_error;
    if (!save_png(ocr_bitmap, temporary_png, &save_error)) {
        return {
            false,
            {},
            L"无法保存临时 OCR 选区图像：" + save_error,
        };
    }
    SetFileAttributesW(
        temporary_png.c_str(),
        FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_TEMPORARY);

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
    command_line += L" --ort-threads 2";

    return run_ocr_process(
        executable,
        std::move(command_line),
        *dependency_lease);
}

}  // namespace airshot
