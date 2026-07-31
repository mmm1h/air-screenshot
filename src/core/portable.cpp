#include "airshot/portable.h"

#include "airshot/config.h"
#include "portable_internal.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <shlwapi.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <wintrust.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#pragma comment(lib, "winhttp.lib")

namespace airshot {
namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"AirScreenshot";
constexpr std::uint64_t kMaxManifestBytes = 64ULL * 1024ULL;
constexpr std::size_t kMaximumUrlCharacters = 4096;
constexpr ULONGLONG kDownloadDeadlineMs = 120'000;
constexpr std::size_t kHttpReadBufferBytes = 8 * 1024;
constexpr int kMaximumRedirects = 5;
constexpr unsigned int kSecurityReopenAttempts = 100;
constexpr DWORD kSecurityReopenRetryDelayMs = 50;
constexpr unsigned long long kMaximumVersionPart = 65'535;
constexpr wchar_t kUpdateMutexPrefix[] = L"Local\\AirScreenshot.Update.v1.";
constexpr std::wstring_view kUpdateReadyEventPrefix =
    L"Local\\AirScreenshot.Update.Ready.";

class UniqueFile {
public:
    UniqueFile() = default;
    explicit UniqueFile(HANDLE value) noexcept : value_(value) {}
    ~UniqueFile() {
        reset();
    }

    UniqueFile(const UniqueFile&) = delete;
    UniqueFile& operator=(const UniqueFile&) = delete;

    UniqueFile(UniqueFile&& other) noexcept : value_(other.release()) {}
    UniqueFile& operator=(UniqueFile&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE result = value_;
        value_ = INVALID_HANDLE_VALUE;
        return result;
    }

    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (*this) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

class UniqueInternetHandle {
public:
    UniqueInternetHandle() = default;
    explicit UniqueInternetHandle(HINTERNET value) noexcept : value_(value) {}
    ~UniqueInternetHandle() {
        reset();
    }

    UniqueInternetHandle(const UniqueInternetHandle&) = delete;
    UniqueInternetHandle& operator=(const UniqueInternetHandle&) = delete;

    UniqueInternetHandle(UniqueInternetHandle&& other) noexcept : value_(other.release()) {}
    UniqueInternetHandle& operator=(UniqueInternetHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HINTERNET get() const noexcept {
        return value_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
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

class ScopedPathCleanup {
public:
    explicit ScopedPathCleanup(std::filesystem::path path) : path_(std::move(path)) {}
    ~ScopedPathCleanup() {
        if (!path_.empty()) {
            DeleteFileW(path_.c_str());
        }
    }

    ScopedPathCleanup(const ScopedPathCleanup&) = delete;
    ScopedPathCleanup& operator=(const ScopedPathCleanup&) = delete;

    void release() noexcept {
        path_.clear();
    }

private:
    std::filesystem::path path_;
};

struct ParsedHttpsUrl {
    std::wstring host;
    std::wstring request_target;
    INTERNET_PORT port{};
};

struct FileIdentity {
    DWORD volume_serial{};
    DWORD index_high{};
    DWORD index_low{};

    [[nodiscard]] bool operator==(const FileIdentity&) const noexcept = default;
};

struct DirectoryTreeGuard {
    std::filesystem::path path;
    std::vector<UniqueFile> handles;

    [[nodiscard]] HANDLE leaf() const noexcept {
        return handles.empty() ? INVALID_HANDLE_VALUE : handles.back().get();
    }
};

struct StableTargetGuard {
    DirectoryTreeGuard directories;
    UniqueFile target;
};

std::optional<FileIdentity> file_identity(HANDLE file);

void clear_error(std::wstring* error) {
    if (error) {
        error->clear();
    }
}

void set_error(std::wstring* error, std::wstring value) {
    if (error) {
        *error = std::move(value);
    }
}

bool cancellation_requested(
    std::stop_token stop_token,
    std::wstring* error) {
    if (!stop_token.stop_requested()) {
        return false;
    }
    set_error(error, L"更新检查已取消。");
    return true;
}

std::timed_mutex& update_mutex() {
    static std::timed_mutex value;
    return value;
}

class UpdateOperationLock {
public:
    UpdateOperationLock() = default;
    ~UpdateOperationLock() {
        if (named_acquired_ && named_mutex_) {
            ReleaseMutex(named_mutex_.get());
        }
    }

    UpdateOperationLock(const UpdateOperationLock&) = delete;
    UpdateOperationLock& operator=(const UpdateOperationLock&) = delete;

    UpdateOperationLock(UpdateOperationLock&& other) noexcept
        : local_(std::move(other.local_)),
          named_mutex_(std::move(other.named_mutex_)),
          named_acquired_(std::exchange(other.named_acquired_, false)) {}

    UpdateOperationLock& operator=(UpdateOperationLock&& other) noexcept {
        if (this != &other) {
            if (named_acquired_ && named_mutex_) {
                ReleaseMutex(named_mutex_.get());
            }
            local_ = std::move(other.local_);
            named_mutex_ = std::move(other.named_mutex_);
            named_acquired_ = std::exchange(other.named_acquired_, false);
        }
        return *this;
    }

    std::unique_lock<std::timed_mutex> local_;
    UniqueFile named_mutex_;
    bool named_acquired_{};
};

struct UpdateMutexSecurity {
    UpdateMutexSecurity() = default;
    UpdateMutexSecurity(const UpdateMutexSecurity&) = delete;
    UpdateMutexSecurity& operator=(
        const UpdateMutexSecurity&) = delete;
    UpdateMutexSecurity(UpdateMutexSecurity&&) noexcept = default;
    UpdateMutexSecurity& operator=(
        UpdateMutexSecurity&&) noexcept = default;

    std::wstring name;
    std::vector<std::byte> user_sid;
    std::vector<std::byte> acl;
    SECURITY_DESCRIPTOR descriptor{};
};

std::optional<UpdateMutexSecurity> current_user_named_object_security(
    DWORD allowed_access,
    std::wstring* error) {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }
    UniqueFile token(raw_token);

    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(TOKEN_USER)) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }
    std::vector<std::byte> storage(bytes);
    if (!GetTokenInformation(
            token.get(), TokenUser, storage.data(), bytes, &bytes)) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }
    const auto* token_user =
        reinterpret_cast<const TOKEN_USER*>(storage.data());
    const DWORD sid_bytes = GetLengthSid(token_user->User.Sid);
    if (sid_bytes == 0 || !IsValidSid(token_user->User.Sid)) {
        set_error(error, L"无法读取当前用户的安全标识。");
        return std::nullopt;
    }
    UpdateMutexSecurity result;
    result.user_sid.resize(sid_bytes);
    if (!CopySid(
            sid_bytes,
            result.user_sid.data(),
            token_user->User.Sid)) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }

    const auto* sid =
        reinterpret_cast<const std::uint8_t*>(
            result.user_sid.data());
    result.name = kUpdateMutexPrefix;
    result.name.reserve(
        result.name.size() +
        static_cast<std::size_t>(sid_bytes) * 2);
    for (DWORD index = 0; index < sid_bytes; ++index) {
        result.name += std::format(L"{:02X}", sid[index]);
    }

    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_sid_storage{};
    DWORD system_sid_bytes =
        static_cast<DWORD>(system_sid_storage.size());
    PSID system_sid = system_sid_storage.data();
    if (!CreateWellKnownSid(
            WinLocalSystemSid,
            nullptr,
            system_sid,
            &system_sid_bytes)) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }
    const DWORD acl_bytes =
        sizeof(ACL) +
        2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) +
        sid_bytes + system_sid_bytes;
    result.acl.resize(acl_bytes);
    auto* acl =
        reinterpret_cast<PACL>(result.acl.data());
    if (!InitializeAcl(acl, acl_bytes, ACL_REVISION) ||
        !AddAccessAllowedAceEx(
            acl,
            ACL_REVISION,
            0,
            allowed_access,
            result.user_sid.data()) ||
        !AddAccessAllowedAceEx(
            acl,
            ACL_REVISION,
            0,
            allowed_access,
            system_sid) ||
        !InitializeSecurityDescriptor(
            &result.descriptor,
            SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(
            &result.descriptor,
            result.user_sid.data(),
            FALSE) ||
        !SetSecurityDescriptorDacl(
            &result.descriptor,
            TRUE,
            acl,
            FALSE)) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }
    return result;
}

std::optional<UpdateOperationLock> lock_update_operation(
    std::stop_token stop_token,
    DWORD timeout_ms,
    std::wstring* error) {
    using namespace std::chrono;

    const auto started = steady_clock::now();
    const auto deadline =
        timeout_ms == INFINITE || timeout_ms == 0
            ? steady_clock::time_point::max()
            : started + milliseconds(timeout_ms);
    const auto remaining_slice = [&deadline]() -> milliseconds {
        if (deadline == steady_clock::time_point::max()) {
            return milliseconds(50);
        }
        const auto remaining =
            duration_cast<milliseconds>(deadline - steady_clock::now());
        return std::clamp(remaining, milliseconds(0), milliseconds(50));
    };
    const auto stopped_or_timed_out =
        [&stop_token, &deadline, error]() {
            if (stop_token.stop_requested()) {
                set_error(error, L"更新检查已取消。");
                return true;
            }
            if (deadline != steady_clock::time_point::max() &&
                steady_clock::now() >= deadline) {
                set_error(error, L"另一个更新操作正在进行。");
                return true;
            }
            return false;
        };

    UpdateOperationLock result;
    result.local_ =
        std::unique_lock<std::timed_mutex>(update_mutex(), std::defer_lock);
    if (timeout_ms == 0) {
        if (!result.local_.try_lock()) {
            set_error(error, L"另一个更新操作正在进行。");
            return std::nullopt;
        }
    } else {
        while (!result.local_.try_lock_for(remaining_slice())) {
            if (stopped_or_timed_out()) {
                return std::nullopt;
            }
        }
    }
    if (stop_token.stop_requested()) {
        set_error(error, L"更新检查已取消。");
        return std::nullopt;
    }

    auto mutex_security =
        current_user_named_object_security(
            MUTEX_ALL_ACCESS, error);
    if (!mutex_security) {
        return std::nullopt;
    }
    SECURITY_ATTRIBUTES security_attributes{
        sizeof(security_attributes),
        &mutex_security->descriptor,
        FALSE};
    result.named_mutex_.reset(
        CreateMutexW(
            &security_attributes,
            FALSE,
            mutex_security->name.c_str()));
    const DWORD create_error = GetLastError();
    if (!result.named_mutex_) {
        set_error(error, windows_error_message(create_error));
        return std::nullopt;
    }
    if (create_error == ERROR_ALREADY_EXISTS) {
        PSID owner = nullptr;
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const DWORD security_error = GetSecurityInfo(
            result.named_mutex_.get(),
            SE_KERNEL_OBJECT,
            OWNER_SECURITY_INFORMATION |
                DACL_SECURITY_INFORMATION,
            &owner,
            nullptr,
            &dacl,
            nullptr,
            &descriptor);
        const bool expected_security =
            security_error == ERROR_SUCCESS &&
            owner && dacl &&
            EqualSid(owner, mutex_security->user_sid.data()) &&
            dacl->AclSize == mutex_security->acl.size() &&
            std::memcmp(
                dacl,
                mutex_security->acl.data(),
                dacl->AclSize) == 0;
        if (descriptor) {
            LocalFree(descriptor);
        }
        if (!expected_security) {
            set_error(
                error,
                L"更新互斥对象的安全描述符无效。");
            return std::nullopt;
        }
    }

    while (true) {
        if (stopped_or_timed_out()) {
            return std::nullopt;
        }
        const auto slice = remaining_slice();
        const DWORD wait_timeout =
            timeout_ms == 0
                ? 0
                : static_cast<DWORD>(
                      std::max<long long>(1, slice.count()));
        const DWORD wait_result =
            WaitForSingleObject(result.named_mutex_.get(), wait_timeout);
        if (wait_result == WAIT_OBJECT_0 ||
            wait_result == WAIT_ABANDONED) {
            result.named_acquired_ = true;
            return result;
        }
        if (wait_result != WAIT_TIMEOUT) {
            set_error(error, windows_error_message(GetLastError()));
            return std::nullopt;
        }
        if (timeout_ms == 0) {
            set_error(error, L"另一个更新操作正在进行。");
            return std::nullopt;
        }
    }
}

std::filesystem::path updates_directory_path() {
    return std::filesystem::absolute(config_directory() / L"updates").lexically_normal();
}

std::filesystem::path pending_manifest_path(const std::filesystem::path& directory) {
    return directory / L"pending.json";
}

struct PendingUpdate {
    UpdateManifest manifest;
    UpdateRequestSource source{UpdateRequestSource::automatic};
};

std::optional<PendingUpdate> parse_pending_update(std::wstring_view json) {
    const auto manifest = parse_update_manifest(json);
    if (!manifest) {
        return std::nullopt;
    }
    try {
        const JsonObject object = JsonObject::Parse(json);
        UpdateRequestSource source = UpdateRequestSource::automatic;
        if (object.HasKey(L"source")) {
            const std::wstring value =
                object.GetNamedString(L"source").c_str();
            if (value == L"manual") {
                source = UpdateRequestSource::manual;
            } else if (value != L"automatic") {
                return std::nullopt;
            }
        }
        return PendingUpdate{*manifest, source};
    } catch (...) {
        return std::nullopt;
    }
}

std::wstring pending_update_to_json(const PendingUpdate& pending) {
    JsonObject object = JsonObject::Parse(
        update_manifest_to_json(pending.manifest));
    object.SetNamedValue(
        L"source",
        JsonValue::CreateStringValue(
            pending.source == UpdateRequestSource::manual
                ? L"manual"
                : L"automatic"));
    return object.Stringify().c_str();
}

std::filesystem::path update_executable_path(
    const std::filesystem::path& directory,
    std::wstring_view version) {
    return directory / std::format(L"AirScreenshot-{}.exe", version);
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

bool is_zero_hash(std::wstring_view value) {
    return value.size() == 64 &&
           std::ranges::all_of(value, [](wchar_t character) { return character == L'0'; });
}

std::optional<std::array<unsigned long long, 3>> parse_version_parts(
    std::wstring_view value) {
    std::array<unsigned long long, 3> result{};
    std::size_t start = 0;
    for (std::size_t part_index = 0; part_index < result.size(); ++part_index) {
        const std::size_t end = value.find(L'.', start);
        if ((part_index + 1 < result.size() && end == std::wstring_view::npos) ||
            (part_index + 1 == result.size() && end != std::wstring_view::npos)) {
            return std::nullopt;
        }
        const std::wstring_view part =
            value.substr(start, end == std::wstring_view::npos ? value.size() - start : end - start);
        if (part.empty() || (part.size() > 1 && part.front() == L'0')) {
            return std::nullopt;
        }
        unsigned long long number = 0;
        for (const wchar_t character : part) {
            if (character < L'0' || character > L'9') {
                return std::nullopt;
            }
            const unsigned int digit = static_cast<unsigned int>(character - L'0');
            if (number > (kMaximumVersionPart - digit) / 10ULL) {
                return std::nullopt;
            }
            number = number * 10ULL + digit;
        }
        result[part_index] = number;
        if (end != std::wstring_view::npos) {
            start = end + 1;
        }
    }
    return result;
}

bool valid_version(std::wstring_view value) {
    return parse_version_parts(value).has_value();
}

bool is_safe_directory(const std::filesystem::path& path) {
    UniqueFile directory(CreateFileW(path.c_str(),
                                     FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr,
                                     OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                     nullptr));
    if (!directory) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    return GetFileInformationByHandleEx(
               directory.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool is_safe_directory_tree(const std::filesystem::path& path) {
    try {
        const std::filesystem::path normalized =
            std::filesystem::absolute(path).lexically_normal();
        std::filesystem::path current = normalized.root_path();
        if (current.empty() || !is_safe_directory(current)) {
            return false;
        }
        for (const auto& component : normalized.relative_path()) {
            current /= component;
            if (!is_safe_directory(current)) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<DirectoryTreeGuard> lock_safe_directory_tree(
    const std::filesystem::path& path,
    std::wstring* error) {
    try {
        DirectoryTreeGuard guard;
        guard.path = std::filesystem::absolute(path).lexically_normal();
        std::filesystem::path current = guard.path.root_path();
        if (current.empty()) {
            set_error(error, L"目录路径不是有效的绝对路径。");
            return std::nullopt;
        }

        const auto lock_component =
            [&guard, error](const std::filesystem::path& component) {
                UniqueFile directory(CreateFileW(
                    component.c_str(),
                    FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr));
                if (!directory) {
                    set_error(error, windows_error_message(GetLastError()));
                    return false;
                }
                FILE_ATTRIBUTE_TAG_INFO attributes{};
                if (!GetFileInformationByHandleEx(
                        directory.get(),
                        FileAttributeTagInfo,
                        &attributes,
                        sizeof(attributes)) ||
                    (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                    (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    set_error(error, L"目录树包含 reparse point 或非常规目录。");
                    return false;
                }
                guard.handles.push_back(std::move(directory));
                return true;
            };

        if (!lock_component(current)) {
            return std::nullopt;
        }
        for (const auto& component : guard.path.relative_path()) {
            current /= component;
            if (!lock_component(current)) {
                return std::nullopt;
            }
        }
        return guard;
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return std::nullopt;
    }
}

bool ensure_safe_directory_tree(
    const std::filesystem::path& path,
    std::wstring* error) {
    try {
        const std::filesystem::path normalized =
            std::filesystem::absolute(path).lexically_normal();
        std::filesystem::path current = normalized.root_path();
        if (current.empty() || !is_safe_directory(current)) {
            set_error(error, L"更新目录的根路径不可访问或是 reparse point。");
            return false;
        }
        for (const auto& component : normalized.relative_path()) {
            current /= component;
            DWORD attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD query_error = GetLastError();
                if (query_error != ERROR_FILE_NOT_FOUND &&
                    query_error != ERROR_PATH_NOT_FOUND) {
                    set_error(error, windows_error_message(query_error));
                    return false;
                }
                if (!CreateDirectoryW(current.c_str(), nullptr) &&
                    GetLastError() != ERROR_ALREADY_EXISTS) {
                    set_error(error, windows_error_message(GetLastError()));
                    return false;
                }
                attributes = GetFileAttributesW(current.c_str());
            }
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                !is_safe_directory(current)) {
                set_error(error, L"更新目录包含 reparse point 或非常规目录。");
                return false;
            }
        }
        return true;
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return false;
    }
}

std::optional<std::filesystem::path> safe_updates_directory(
    bool create,
    std::wstring* error = nullptr) {
    try {
        const std::filesystem::path directory = updates_directory_path();
        const std::filesystem::path parent = directory.parent_path();
        std::error_code filesystem_error;
        if (create && !ensure_safe_directory_tree(directory, error)) {
            return std::nullopt;
        }
        if (filesystem_error || !std::filesystem::exists(directory, filesystem_error) ||
            filesystem_error || !is_safe_directory_tree(parent) ||
            !is_safe_directory_tree(directory)) {
            set_error(error, L"更新目录不存在、不可访问或包含 reparse point。");
            return std::nullopt;
        }
        return directory;
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> unique_path(
    const std::filesystem::path& directory,
    std::wstring_view purpose,
    std::wstring_view extension) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::uint64_t random = 0;
        if (BCryptGenRandom(nullptr,
                            reinterpret_cast<PUCHAR>(&random),
                            static_cast<ULONG>(sizeof(random)),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return std::nullopt;
        }
        const auto candidate = directory /
                               std::format(L".airshot-{}-{}-{}-{:016X}{}",
                                           purpose,
                                           GetCurrentProcessId(),
                                           GetCurrentThreadId(),
                                           random,
                                           extension);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND) {
            return candidate;
        }
    }
    return std::nullopt;
}

UniqueFile create_update_ready_event(
    DWORD process_id,
    std::wstring* name,
    std::wstring* error) {
    auto event_security =
        current_user_named_object_security(
            EVENT_ALL_ACCESS, error);
    if (!event_security) {
        return {};
    }
    SECURITY_ATTRIBUTES security_attributes{
        sizeof(security_attributes),
        &event_security->descriptor,
        FALSE};
    std::uint64_t random = 0;
    if (BCryptGenRandom(nullptr,
                        reinterpret_cast<PUCHAR>(&random),
                        static_cast<ULONG>(sizeof(random)),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        set_error(error, L"无法生成更新握手标识。");
        return {};
    }
    const std::wstring candidate =
        std::format(L"{}{}.{:016X}",
                    kUpdateReadyEventPrefix,
                    process_id,
                    random);
    UniqueFile event(
        CreateEventW(
            &security_attributes,
            TRUE,
            FALSE,
            candidate.c_str()));
    if (!event) {
        set_error(error, windows_error_message(GetLastError()));
        return {};
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        set_error(
            error,
            L"更新握手事件名称已被占用。");
        return {};
    }
    if (name) {
        *name = candidate;
    }
    return event;
}

bool valid_update_ready_event_name(
    std::wstring_view value,
    DWORD process_id) {
    const std::wstring expected_prefix =
        std::format(L"{}{}.", kUpdateReadyEventPrefix, process_id);
    if (!value.starts_with(expected_prefix) ||
        value.size() != expected_prefix.size() + 16) {
        return false;
    }
    return std::ranges::all_of(
        value.substr(expected_prefix.size()),
        [](wchar_t character) {
            return (character >= L'0' && character <= L'9') ||
                   (character >= L'A' && character <= L'F');
        });
}

UniqueFile open_regular_file_with_access(
    const std::filesystem::path& path,
    DWORD access,
    DWORD sharing,
    std::wstring* error,
    unsigned int open_attempts = 1) {
    UniqueFile file;
    DWORD open_error = ERROR_SUCCESS;
    open_attempts = std::max(1U, open_attempts);
    for (unsigned int attempt = 0; attempt < open_attempts; ++attempt) {
        file.reset(CreateFileW(
            path.c_str(),
            access,
            sharing,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL |
                FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (file) {
            break;
        }
        open_error = GetLastError();
        if ((open_error != ERROR_SHARING_VIOLATION &&
             open_error != ERROR_LOCK_VIOLATION) ||
            attempt + 1U >= open_attempts) {
            break;
        }
        Sleep(kSecurityReopenRetryDelayMs);
    }
    if (!file) {
        set_error(error, windows_error_message(open_error));
        return {};
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0 ||
        GetFileType(file.get()) != FILE_TYPE_DISK) {
        set_error(error, L"更新文件不是安全的常规文件。");
        return {};
    }
    return file;
}

UniqueFile open_regular_file(
    const std::filesystem::path& path,
    DWORD sharing,
    std::wstring* error) {
    return open_regular_file_with_access(
        path, GENERIC_READ, sharing, error);
}

std::optional<FileIdentity> file_identity(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) {
        return std::nullopt;
    }
    return FileIdentity{
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow,
    };
}

bool path_matches_directory_guard(
    const std::filesystem::path& path,
    const DirectoryTreeGuard& guard) {
    try {
        const std::wstring candidate =
            std::filesystem::absolute(path).lexically_normal().wstring();
        const std::wstring expected = guard.path.wstring();
        return CompareStringOrdinal(
                   candidate.c_str(),
                   static_cast<int>(candidate.size()),
                   expected.c_str(),
                   static_cast<int>(expected.size()),
                   TRUE) == CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

bool revalidate_directory_guard(
    const DirectoryTreeGuard& guard,
    std::wstring* error) {
    if (guard.handles.empty() || !guard.leaf()) {
        set_error(error, L"更新目录 guard 无效。");
        return false;
    }
    UniqueFile current(CreateFileW(
        guard.path.c_str(),
        FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!current) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    const auto expected_identity = file_identity(guard.leaf());
    const auto current_identity = file_identity(current.get());
    if (!GetFileInformationByHandleEx(
            current.get(),
            FileAttributeTagInfo,
            &attributes,
            sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !expected_identity || !current_identity ||
        *expected_identity != *current_identity) {
        set_error(error, L"更新目录在操作期间发生了变化。");
        return false;
    }
    return true;
}

std::optional<std::uint64_t> file_size(HANDLE file) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

constexpr SECURITY_INFORMATION kPortableFileSecurityInformation =
    OWNER_SECURITY_INFORMATION |
    GROUP_SECURITY_INFORMATION |
    DACL_SECURITY_INFORMATION |
    LABEL_SECURITY_INFORMATION;

struct SecurityDescriptorSnapshot {
    std::vector<std::byte> bytes;

    [[nodiscard]] PSECURITY_DESCRIPTOR get() const noexcept {
        return bytes.empty()
                   ? nullptr
                   : const_cast<std::byte*>(bytes.data());
    }
};

std::optional<SecurityDescriptorSnapshot> capture_security_descriptor(
    HANDLE file,
    std::wstring* error) {
    DWORD bytes = 0;
    GetKernelObjectSecurity(
        file,
        kPortableFileSecurityInformation,
        nullptr,
        0,
        &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        bytes < SECURITY_DESCRIPTOR_MIN_LENGTH) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }
    SecurityDescriptorSnapshot result;
    result.bytes.resize(bytes);
    if (!GetKernelObjectSecurity(
            file,
            kPortableFileSecurityInformation,
            result.get(),
            bytes,
            &bytes) ||
        !IsValidSecurityDescriptor(result.get())) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }
    result.bytes.resize(bytes);
    return result;
}

bool security_descriptors_equivalent(
    const SecurityDescriptorSnapshot& first,
    const SecurityDescriptorSnapshot& second,
    std::wstring* difference = nullptr) {
    const auto mismatch =
        [difference](std::wstring value) {
            if (difference) {
                *difference = std::move(value);
            }
            return false;
        };
    PSID first_owner = nullptr;
    PSID second_owner = nullptr;
    PSID first_group = nullptr;
    PSID second_group = nullptr;
    BOOL ignored_defaulted = FALSE;
    if (!GetSecurityDescriptorOwner(
            first.get(), &first_owner, &ignored_defaulted) ||
        !GetSecurityDescriptorOwner(
            second.get(), &second_owner, &ignored_defaulted) ||
        !first_owner || !second_owner ||
        !EqualSid(first_owner, second_owner)) {
        return mismatch(L"owner 不一致");
    }
    if (!GetSecurityDescriptorGroup(
            first.get(), &first_group, &ignored_defaulted) ||
        !GetSecurityDescriptorGroup(
            second.get(), &second_group, &ignored_defaulted) ||
        !first_group || !second_group ||
        !EqualSid(first_group, second_group)) {
        return mismatch(L"primary group 不一致");
    }

    BOOL first_dacl_present = FALSE;
    BOOL second_dacl_present = FALSE;
    PACL first_dacl = nullptr;
    PACL second_dacl = nullptr;
    if (!GetSecurityDescriptorDacl(
            first.get(),
            &first_dacl_present,
            &first_dacl,
            &ignored_defaulted) ||
        !GetSecurityDescriptorDacl(
            second.get(),
            &second_dacl_present,
            &second_dacl,
            &ignored_defaulted) ||
        first_dacl_present != second_dacl_present ||
        (first_dacl == nullptr) != (second_dacl == nullptr)) {
        return mismatch(L"DACL presence 不一致");
    }
    if (first_dacl && second_dacl &&
        (first_dacl->AclSize != second_dacl->AclSize ||
         std::memcmp(
             first_dacl,
             second_dacl,
             first_dacl->AclSize) != 0)) {
        return mismatch(L"DACL 内容不一致");
    }

    BOOL first_sacl_present = FALSE;
    BOOL second_sacl_present = FALSE;
    PACL first_sacl = nullptr;
    PACL second_sacl = nullptr;
    if (!GetSecurityDescriptorSacl(
            first.get(),
            &first_sacl_present,
            &first_sacl,
            &ignored_defaulted) ||
        !GetSecurityDescriptorSacl(
            second.get(),
            &second_sacl_present,
            &second_sacl,
            &ignored_defaulted) ||
        (first_sacl == nullptr) != (second_sacl == nullptr)) {
        return mismatch(
            std::format(
                L"mandatory label SACL presence 不一致（expected present={} null={}；actual present={} null={}）",
                first_sacl_present,
                first_sacl == nullptr,
                second_sacl_present,
                second_sacl == nullptr));
    }
    // In the LABEL_SECURITY_INFORMATION projection, absent and present-null
    // forms both contain no mandatory-label ACE and are equivalent.
    if ((first_sacl || second_sacl) &&
        first_sacl_present != second_sacl_present) {
        return mismatch(
            L"mandatory label SACL presence 不一致");
    }
    if (first_sacl && second_sacl &&
        (first_sacl->AclSize != second_sacl->AclSize ||
         std::memcmp(
             first_sacl,
             second_sacl,
             first_sacl->AclSize) != 0)) {
        return mismatch(L"mandatory label SACL 内容不一致");
    }

    SECURITY_DESCRIPTOR_CONTROL first_control{};
    SECURITY_DESCRIPTOR_CONTROL second_control{};
    DWORD first_revision = 0;
    DWORD second_revision = 0;
    if (!GetSecurityDescriptorControl(
            first.get(), &first_control, &first_revision) ||
        !GetSecurityDescriptorControl(
            second.get(), &second_control, &second_revision)) {
        return mismatch(L"security descriptor control 无法读取");
    }
    constexpr SECURITY_DESCRIPTOR_CONTROL protection_control_mask =
        SE_DACL_PROTECTED | SE_SACL_PROTECTED;
    if ((first_control & protection_control_mask) !=
        (second_control & protection_control_mask)) {
        return mismatch(L"DACL/SACL protection control 不一致");
    }
    return true;
}

bool apply_security_descriptor(
    HANDLE file,
    const SecurityDescriptorSnapshot& snapshot,
    std::wstring* error) {
    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    PACL sacl = nullptr;
    PACL current_sacl = nullptr;
    BOOL dacl_present = FALSE;
    BOOL sacl_present = FALSE;
    BOOL current_sacl_present = FALSE;
    BOOL defaulted = FALSE;
    BOOL current_defaulted = FALSE;
    SECURITY_DESCRIPTOR_CONTROL control{};
    SECURITY_DESCRIPTOR_CONTROL current_control{};
    DWORD revision = 0;
    DWORD current_revision = 0;
    const auto current =
        capture_security_descriptor(file, error);
    if (!GetSecurityDescriptorOwner(
            snapshot.get(), &owner, &defaulted) ||
        !GetSecurityDescriptorGroup(
            snapshot.get(), &group, &defaulted) ||
        !GetSecurityDescriptorDacl(
            snapshot.get(),
            &dacl_present,
            &dacl,
            &defaulted) ||
        !GetSecurityDescriptorSacl(
            snapshot.get(),
            &sacl_present,
            &sacl,
            &defaulted) ||
        !GetSecurityDescriptorControl(
            snapshot.get(), &control, &revision) ||
        !current ||
        !GetSecurityDescriptorSacl(
            current->get(),
            &current_sacl_present,
            &current_sacl,
            &current_defaulted) ||
        !GetSecurityDescriptorControl(
            current->get(),
            &current_control,
            &current_revision)) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    const bool update_label =
        sacl_present || sacl != nullptr ||
        current_sacl != nullptr;
    SECURITY_INFORMATION information =
        OWNER_SECURITY_INFORMATION |
        GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION;
    if (update_label) {
        information |= LABEL_SECURITY_INFORMATION;
    }
    information |=
        (control & SE_DACL_PROTECTED) != 0
            ? PROTECTED_DACL_SECURITY_INFORMATION
            : UNPROTECTED_DACL_SECURITY_INFORMATION;
    // SACL protection transitions require privileges that an asInvoker process
    // may not have. Preserve them strictly and fail closed instead of silently
    // weakening the original mandatory-label inheritance policy.
    if (update_label &&
        ((control ^ current_control) &
         SE_SACL_PROTECTED) != 0) {
        information |=
            (control & SE_SACL_PROTECTED) != 0
                ? PROTECTED_SACL_SECURITY_INFORMATION
                : UNPROTECTED_SACL_SECURITY_INFORMATION;
    }
    const DWORD security_error = SetSecurityInfo(
        file,
        SE_FILE_OBJECT,
        information,
        owner,
        group,
        dacl,
        sacl);
    if (security_error != ERROR_SUCCESS) {
        set_error(error, windows_error_message(security_error));
        return false;
    }
    SECURITY_INFORMATION exact_information =
        DACL_SECURITY_INFORMATION;
    if (update_label) {
        exact_information |= LABEL_SECURITY_INFORMATION;
    }
    if (!SetKernelObjectSecurity(
            file,
            exact_information,
            snapshot.get())) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    return true;
}

bool verify_security_descriptor(
    HANDLE file,
    const SecurityDescriptorSnapshot& expected,
    std::wstring* error) {
    const auto actual = capture_security_descriptor(file, error);
    std::wstring difference;
    if (!actual ||
        !security_descriptors_equivalent(
            expected, *actual, &difference)) {
        if (error && error->empty()) {
            *error = difference.empty()
                         ? L"更新未能保留原程序的安全描述符。"
                         : L"更新未能保留原程序的安全描述符：" +
                               difference + L"。";
        }
        return false;
    }
    return true;
}

bool ensure_security_descriptor(
    const std::filesystem::path& path,
    const SecurityDescriptorSnapshot& expected,
    UniqueFile* file_guard,
    std::wstring* error) {
    if (!file_guard || !*file_guard) {
        set_error(error, L"安全描述符校验句柄无效。");
        return false;
    }
    if (verify_security_descriptor(
            file_guard->get(), expected, nullptr)) {
        return true;
    }

    const auto expected_identity =
        file_identity(file_guard->get());
    if (!expected_identity) {
        set_error(
            error,
            L"无法在安全描述符修复前确认更新文件身份。");
        return false;
    }
    file_guard->reset();
    *file_guard = open_regular_file_with_access(
        path,
        GENERIC_READ | GENERIC_WRITE | READ_CONTROL |
            WRITE_DAC | WRITE_OWNER,
        FILE_SHARE_READ,
        error,
        kSecurityReopenAttempts);
    const auto reopened_identity =
        *file_guard
            ? file_identity(file_guard->get())
            : std::nullopt;
    if (!*file_guard) {
        return false;
    }
    if (!reopened_identity ||
        *reopened_identity != *expected_identity) {
        set_error(
            error,
            L"更新文件在安全描述符修复重开期间发生了变化。");
        file_guard->reset();
        return false;
    }
    if (
        !apply_security_descriptor(
            file_guard->get(), expected, error)) {
        return false;
    }
    if (!FlushFileBuffers(file_guard->get())) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    if (!verify_security_descriptor(
            file_guard->get(), expected, error)) {
        return false;
    }

    file_guard->reset();
    *file_guard = open_regular_file_with_access(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        error,
        kSecurityReopenAttempts);
    const auto read_guard_identity =
        *file_guard
            ? file_identity(file_guard->get())
            : std::nullopt;
    if (!*file_guard) {
        return false;
    }
    if (!read_guard_identity ||
        *read_guard_identity != *expected_identity) {
        set_error(
            error,
            L"更新文件在安全描述符修复后的只读重开期间发生了变化。");
        file_guard->reset();
        return false;
    }
    return verify_security_descriptor(
        file_guard->get(), expected, error);
}

std::optional<bool> volume_supports_persistent_acls(
    const std::filesystem::path& path,
    std::wstring* error) {
    std::wstring volume_path(32'768, L'\0');
    if (!GetVolumePathNameW(
            path.c_str(),
            volume_path.data(),
            static_cast<DWORD>(volume_path.size()))) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }
    volume_path.resize(
        wcsnlen_s(volume_path.c_str(), volume_path.size()));
    DWORD flags = 0;
    if (!GetVolumeInformationW(
            volume_path.c_str(),
            nullptr,
            0,
            nullptr,
            nullptr,
            &flags,
            nullptr,
            0)) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }
    return (flags & FILE_PERSISTENT_ACLS) != 0;
}

bool same_file(
    const std::filesystem::path& first,
    const std::filesystem::path& second,
    std::wstring* error = nullptr) {
    UniqueFile first_file = open_regular_file(first, FILE_SHARE_READ, error);
    if (!first_file) {
        return false;
    }
    UniqueFile second_file = open_regular_file(second, FILE_SHARE_READ, error);
    if (!second_file) {
        return false;
    }
    const auto first_identity = file_identity(first_file.get());
    const auto second_identity = file_identity(second_file.get());
    return first_identity && second_identity && *first_identity == *second_identity;
}

bool safe_existing_destination(const std::filesystem::path& path, std::wstring* error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD last_error = GetLastError();
        if (last_error == ERROR_FILE_NOT_FOUND || last_error == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        set_error(error, windows_error_message(last_error));
        return false;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        set_error(error, L"更新目标是目录或 reparse point。");
        return false;
    }
    return true;
}

bool path_is_missing(const std::filesystem::path& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool atomic_move_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const DirectoryTreeGuard& directory_guard,
    std::wstring* error) {
    if (!path_matches_directory_guard(
            destination.parent_path(), directory_guard) ||
        !revalidate_directory_guard(directory_guard, error) ||
        !safe_existing_destination(destination, error)) {
        if (error && error->empty()) {
            *error = L"更新目标不在受控目录中。";
        }
        return false;
    }
    if (!MoveFileExW(source.c_str(),
                     destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    return revalidate_directory_guard(directory_guard, error);
}

bool write_all(HANDLE file, std::span<const std::byte> bytes, std::wstring* error) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, requested, &written, nullptr) ||
            written == 0) {
            set_error(error, windows_error_message(GetLastError()));
            return false;
        }
        offset += written;
    }
    return true;
}

bool write_text_file(
    const std::filesystem::path& path,
    std::wstring_view text,
    const DirectoryTreeGuard& directory_guard,
    std::wstring* error) {
    try {
        if (!path_matches_directory_guard(
                path.parent_path(), directory_guard) ||
            !revalidate_directory_guard(directory_guard, error) ||
            !safe_existing_destination(path, error)) {
            if (error && error->empty()) {
                *error = L"更新元数据不在受控目录中。";
            }
            return false;
        }
        const auto temporary = unique_path(path.parent_path(), L"metadata", L".tmp");
        if (!temporary) {
            set_error(error, L"无法创建唯一的更新元数据临时文件。");
            return false;
        }
        ScopedPathCleanup cleanup(*temporary);
        UniqueFile file(CreateFileW(temporary->c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH,
                                    nullptr));
        if (!file) {
            set_error(error, windows_error_message(GetLastError()));
            return false;
        }
        if (!revalidate_directory_guard(directory_guard, error)) {
            return false;
        }
        const std::string utf8 = to_utf8(text);
        if (utf8.empty() || utf8.size() > kMaxManifestBytes ||
            !write_all(file.get(),
                       std::as_bytes(std::span<const char>(utf8.data(), utf8.size())),
                       error) ||
            !FlushFileBuffers(file.get())) {
            if (error && error->empty()) {
                *error = L"无法完整写入更新元数据。";
            }
            return false;
        }
        file.reset();
        if (!atomic_move_file(
                *temporary, path, directory_guard, error)) {
            return false;
        }
        cleanup.release();
        return true;
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return false;
    }
}

std::optional<std::wstring> read_text_file(
    const std::filesystem::path& path,
    std::wstring* error = nullptr) {
    try {
        UniqueFile file = open_regular_file(path, FILE_SHARE_READ, error);
        if (!file) {
            return std::nullopt;
        }
        const auto size = file_size(file.get());
        if (!size || *size == 0 || *size > kMaxManifestBytes) {
            set_error(error, L"更新元数据大小无效。");
            return std::nullopt;
        }
        std::string bytes(static_cast<std::size_t>(*size), '\0');
        DWORD read = 0;
        if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
            read != bytes.size()) {
            set_error(error, L"更新元数据读取不完整。");
            return std::nullopt;
        }
        const std::wstring text = from_utf8(bytes);
        if (text.empty()) {
            set_error(error, L"更新元数据不是有效的 UTF-8。");
            return std::nullopt;
        }
        return text;
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return std::nullopt;
    }
}

bool parse_https_url(
    std::wstring_view url,
    ParsedHttpsUrl* parsed = nullptr,
    std::wstring* error = nullptr) {
    if (url.empty() || url.size() > kMaximumUrlCharacters ||
        std::ranges::any_of(url, [](wchar_t character) {
            return character <= 0x20 || character == 0x7F || character == L'\\';
        }) ||
        url.find(L'#') != std::wstring_view::npos) {
        set_error(error, L"下载地址不是有效的 HTTPS URL。");
        return false;
    }

    std::wstring owned(url);
    URL_COMPONENTS components{sizeof(components)};
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(
            owned.c_str(), static_cast<DWORD>(owned.size()), 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.dwHostNameLength == 0 || components.dwUserNameLength != 0 ||
        components.dwPasswordLength != 0 || components.nPort == 0) {
        set_error(error, L"下载地址不是有效的 HTTPS URL。");
        return false;
    }

    if (parsed) {
        parsed->host.assign(components.lpszHostName, components.dwHostNameLength);
        parsed->port = components.nPort;
        if (components.dwUrlPathLength == 0) {
            parsed->request_target = L"/";
        } else {
            parsed->request_target.assign(
                components.lpszUrlPath, components.dwUrlPathLength);
        }
        if (components.dwExtraInfoLength != 0) {
            parsed->request_target.append(
                components.lpszExtraInfo, components.dwExtraInfoLength);
        }
    }
    return true;
}

std::optional<std::wstring> redirect_location(HINTERNET request, std::wstring* error) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_LOCATION,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER,
                        &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t) ||
        bytes > (kMaximumUrlCharacters + 1) * sizeof(wchar_t)) {
        set_error(error, L"服务器返回了无效的重定向地址。");
        return std::nullopt;
    }
    std::wstring location(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_LOCATION,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             location.data(),
                             &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        set_error(error, windows_error_message(GetLastError()));
        return std::nullopt;
    }
    location.resize(wcsnlen_s(location.c_str(), location.size()));
    return location;
}

std::optional<std::wstring> combine_redirect_url(
    std::wstring_view current,
    std::wstring_view location,
    std::wstring* error) {
    if (location.empty() || location.size() > kMaximumUrlCharacters) {
        set_error(error, L"服务器返回了无效的重定向地址。");
        return std::nullopt;
    }
    std::wstring current_owned(current);
    std::wstring location_owned(location);
    std::wstring combined(kMaximumUrlCharacters + 1, L'\0');
    DWORD characters = static_cast<DWORD>(combined.size());
    const HRESULT result = UrlCombineW(
        current_owned.c_str(), location_owned.c_str(), combined.data(), &characters, 0);
    if (FAILED(result) || characters == 0 || characters > kMaximumUrlCharacters) {
        set_error(error, L"服务器返回了无效的重定向地址。");
        return std::nullopt;
    }
    combined.resize(characters);
    if (!parse_https_url(combined, nullptr, error)) {
        set_error(error, L"更新下载拒绝跳转到非 HTTPS 地址。");
        return std::nullopt;
    }
    return combined;
}

bool redirect_status(DWORD status) {
    return status == HTTP_STATUS_MOVED || status == HTTP_STATUS_REDIRECT ||
           status == HTTP_STATUS_REDIRECT_METHOD || status == 307 || status == 308;
}

std::optional<std::uint64_t> response_content_length(HINTERNET request) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_CONTENT_LENGTH,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER,
                        &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t) ||
        bytes > 64 * sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_CONTENT_LENGTH,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             text.data(),
                             &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    text.resize(wcsnlen_s(text.c_str(), text.size()));
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        const unsigned int digit = static_cast<unsigned int>(character - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10ULL) {
            return std::nullopt;
        }
        value = value * 10ULL + digit;
    }
    return value;
}

struct AsyncHttpContext {
    std::atomic<unsigned long> references{1};
    UniqueFile operation_event{
        CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::array<std::byte, kHttpReadBufferBytes> read_buffer{};
    std::atomic<DWORD> status{};
    std::atomic<DWORD> error{};
    std::atomic<DWORD> information_length{};
};

void retain_async_http_context(AsyncHttpContext* context) noexcept {
    context->references.fetch_add(1, std::memory_order_relaxed);
}

void release_async_http_context(AsyncHttpContext* context) noexcept {
    if (context &&
        context->references.fetch_sub(
            1, std::memory_order_acq_rel) == 1) {
        delete context;
    }
}

void CALLBACK async_http_status_callback(
    HINTERNET,
    DWORD_PTR context_value,
    DWORD status,
    void* status_information,
    DWORD status_information_length) {
    auto* context =
        reinterpret_cast<AsyncHttpContext*>(context_value);
    if (!context) {
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        release_async_http_context(context);
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        DWORD operation_error = ERROR_GEN_FAILURE;
        if (status_information &&
            status_information_length >= sizeof(WINHTTP_ASYNC_RESULT)) {
            operation_error =
                static_cast<const WINHTTP_ASYNC_RESULT*>(
                    status_information)
                    ->dwError;
        }
        context->error.store(operation_error, std::memory_order_relaxed);
        context->information_length.store(
            0, std::memory_order_relaxed);
        context->status.store(status, std::memory_order_release);
        SetEvent(context->operation_event.get());
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
        status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
        status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
        context->error.store(ERROR_SUCCESS, std::memory_order_relaxed);
        context->information_length.store(
            status_information_length, std::memory_order_relaxed);
        context->status.store(status, std::memory_order_release);
        SetEvent(context->operation_event.get());
    }
}

class AsyncHttpRequest {
public:
    AsyncHttpRequest() = default;
    ~AsyncHttpRequest() {
        close();
        release_async_http_context(
            std::exchange(context_, nullptr));
    }

    AsyncHttpRequest(const AsyncHttpRequest&) = delete;
    AsyncHttpRequest& operator=(const AsyncHttpRequest&) = delete;

    [[nodiscard]] bool initialize(
        HINTERNET request,
        std::wstring* error) {
        if (!request) {
            set_error(error, windows_error_message(GetLastError()));
            return false;
        }
        std::unique_ptr<AsyncHttpContext> context(
            new (std::nothrow) AsyncHttpContext);
        if (!context || !context->operation_event) {
            if (request) {
                WinHttpCloseHandle(request);
            }
            set_error(error, L"无法初始化异步下载上下文。");
            return false;
        }
        request_ = request;
        context_ = context.release();
        DWORD_PTR context_value =
            reinterpret_cast<DWORD_PTR>(context_);
        if (!WinHttpSetOption(
                request_,
                WINHTTP_OPTION_CONTEXT_VALUE,
                &context_value,
                sizeof(context_value))) {
            set_error(error, windows_error_message(GetLastError()));
            WinHttpCloseHandle(std::exchange(request_, nullptr));
            return false;
        }
        constexpr DWORD callback_flags =
            WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
            WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
            WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
            WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
            WINHTTP_CALLBACK_FLAG_HANDLES;
        retain_async_http_context(context_);
        if (WinHttpSetStatusCallback(
                request_,
                async_http_status_callback,
                callback_flags,
                0) == WINHTTP_INVALID_STATUS_CALLBACK) {
            release_async_http_context(context_);
            set_error(error, windows_error_message(GetLastError()));
            WinHttpCloseHandle(std::exchange(request_, nullptr));
            return false;
        }
        return true;
    }

    [[nodiscard]] HINTERNET get() const noexcept {
        return request_;
    }

    [[nodiscard]] DWORD_PTR context_value() noexcept {
        return reinterpret_cast<DWORD_PTR>(context_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return request_ != nullptr;
    }

    void prepare_operation() {
        ResetEvent(context_->operation_event.get());
        context_->status.store(0, std::memory_order_relaxed);
        context_->error.store(ERROR_SUCCESS, std::memory_order_relaxed);
        context_->information_length.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] HANDLE operation_event() const noexcept {
        return context_->operation_event.get();
    }

    [[nodiscard]] DWORD status() const noexcept {
        return context_->status.load(std::memory_order_acquire);
    }

    [[nodiscard]] DWORD error() const noexcept {
        return context_->error.load(std::memory_order_relaxed);
    }

    [[nodiscard]] DWORD information_length() const noexcept {
        return context_->information_length.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::span<std::byte> read_buffer() noexcept {
        return context_->read_buffer;
    }

    void close() noexcept {
        if (!request_) {
            return;
        }
        const HINTERNET request = std::exchange(request_, nullptr);
        WinHttpCloseHandle(request);
    }

private:
    HINTERNET request_{};
    AsyncHttpContext* context_{};
};

struct AsyncHttpOperationResult {
    bool succeeded{};
    bool cancelled{};
    bool timed_out{};
    DWORD error{};
    DWORD information_length{};
};

template <typename Function>
AsyncHttpOperationResult run_async_http_operation(
    AsyncHttpRequest& request,
    DWORD expected_status,
    HANDLE stop_event,
    std::stop_token stop_token,
    std::chrono::steady_clock::time_point deadline,
    Function&& start_operation) {
    request.prepare_operation();
    if (stop_token.stop_requested()) {
        request.close();
        return {false, true, false, ERROR_CANCELLED, 0};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        request.close();
        return {false, false, true, ERROR_TIMEOUT, 0};
    }
    const BOOL started = start_operation();
    const DWORD start_error =
        started ? ERROR_SUCCESS : GetLastError();
    if (!started && start_error != ERROR_IO_PENDING) {
        return {false, false, false, start_error, 0};
    }

    while (true) {
        if (stop_token.stop_requested()) {
            request.close();
            return {false, true, false, ERROR_CANCELLED, 0};
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            request.close();
            return {false, false, true, ERROR_TIMEOUT, 0};
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
        const DWORD wait_ms = static_cast<DWORD>(
            std::clamp<long long>(
                remaining.count(),
                1,
                std::numeric_limits<DWORD>::max()));
        const std::array<HANDLE, 2> events{
            request.operation_event(), stop_event};
        const DWORD wait_result =
            WaitForMultipleObjects(
                static_cast<DWORD>(events.size()),
                events.data(),
                FALSE,
                wait_ms);
        if (wait_result == WAIT_OBJECT_0) {
            if (stop_token.stop_requested()) {
                request.close();
                return {false, true, false, ERROR_CANCELLED, 0};
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                request.close();
                return {false, false, true, ERROR_TIMEOUT, 0};
            }
            const DWORD status = request.status();
            if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
                return {
                    false,
                    false,
                    false,
                    request.error(),
                    request.information_length()};
            }
            if (status != expected_status) {
                return {
                    false,
                    false,
                    false,
                    ERROR_INVALID_DATA,
                    request.information_length()};
            }
            return {
                true,
                false,
                false,
                ERROR_SUCCESS,
                request.information_length()};
        }
        if (wait_result == WAIT_OBJECT_0 + 1) {
            request.close();
            return {false, true, false, ERROR_CANCELLED, 0};
        }
        if (wait_result == WAIT_TIMEOUT) {
            request.close();
            return {false, false, true, ERROR_TIMEOUT, 0};
        }
        const DWORD wait_error = GetLastError();
        request.close();
        return {false, false, false, wait_error, 0};
    }
}

bool report_async_http_failure(
    const AsyncHttpOperationResult& result,
    std::wstring* error) {
    if (result.cancelled) {
        set_error(error, L"更新检查已取消。");
    } else if (result.timed_out) {
        set_error(error, L"更新下载超时。");
    } else {
        set_error(error,
                  std::format(
                      L"下载失败：{} ({})",
                      windows_error_message(result.error),
                      result.error));
    }
    return false;
}

bool configure_request(
    HINTERNET request,
    DWORD remaining_ms,
    std::wstring* error) {
    const int timeout = static_cast<int>(
        std::min<DWORD>(
            remaining_ms,
            static_cast<DWORD>(std::numeric_limits<int>::max())));
    if (timeout <= 0 ||
        !WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout)) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request,
                          WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirect_policy,
                          sizeof(redirect_policy))) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    DWORD disabled_features = WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(request,
                          WINHTTP_OPTION_DISABLE_FEATURE,
                          &disabled_features,
                          sizeof(disabled_features))) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    DWORD enabled_features = WINHTTP_ENABLE_SSL_REVOCATION;
    if (!WinHttpSetOption(request,
                          WINHTTP_OPTION_ENABLE_FEATURE,
                          &enabled_features,
                          sizeof(enabled_features))) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    return true;
}

bool download_file(
    std::wstring_view url,
    const std::filesystem::path& path,
    std::uint64_t maximum_size,
    std::optional<std::uint64_t> expected_size,
    const DirectoryTreeGuard& directory_guard,
    std::stop_token stop_token,
    std::wstring* error) {
    clear_error(error);
    if (cancellation_requested(stop_token, error)) {
        return false;
    }
    if (maximum_size == 0 || maximum_size > kMaxPortableUpdateBytes ||
        expected_size && (*expected_size == 0 || *expected_size > maximum_size) ||
        !path_matches_directory_guard(
            path.parent_path(), directory_guard) ||
        !revalidate_directory_guard(directory_guard, error)) {
        set_error(error, L"下载目标或大小限制无效。");
        return false;
    }

    ParsedHttpsUrl parsed;
    if (!parse_https_url(url, &parsed, error)) {
        return false;
    }

    UniqueInternetHandle session(WinHttpOpen(L"AirScreenshot Updater/1",
                                             WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                             WINHTTP_NO_PROXY_NAME,
                                             WINHTTP_NO_PROXY_BYPASS,
                                             WINHTTP_FLAG_ASYNC));
    if (!session) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }

    UniqueFile stop_event(
        CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_event) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    std::stop_callback stop_callback(
        stop_token, [event = stop_event.get()] { SetEvent(event); });
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kDownloadDeadlineMs);
    std::wstring current_url(url);
    UniqueInternetHandle response_connection;
    std::unique_ptr<AsyncHttpRequest> response;
    for (int redirect_count = 0; redirect_count <= kMaximumRedirects; ++redirect_count) {
        if (cancellation_requested(stop_token, error)) {
            return false;
        }
        if (!parse_https_url(current_url, &parsed, error)) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            set_error(error, L"更新下载超时。");
            return false;
        }

        UniqueInternetHandle connection(
            WinHttpConnect(session.get(), parsed.host.c_str(), parsed.port, 0));
        if (!connection) {
            set_error(error, windows_error_message(GetLastError()));
            return false;
        }
        const HINTERNET raw_request =
            WinHttpOpenRequest(connection.get(),
                               L"GET",
                               parsed.request_target.c_str(),
                               nullptr,
                               WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                               WINHTTP_FLAG_SECURE);
        auto request = std::make_unique<AsyncHttpRequest>();
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
        if (!raw_request ||
            !request->initialize(raw_request, error) ||
            !configure_request(
                request->get(),
                static_cast<DWORD>(std::clamp<long long>(
                    remaining.count(),
                    1,
                    std::numeric_limits<DWORD>::max())),
                error)) {
            if (error && error->empty()) {
                *error = windows_error_message(GetLastError());
                if (error->empty()) {
                    *error = L"无法初始化更新下载请求。";
                }
            }
            return false;
        }

        constexpr wchar_t headers[] =
            L"Accept: application/octet-stream\r\n"
            L"Accept-Encoding: identity\r\n"
            L"Cache-Control: no-cache\r\n";
        const AsyncHttpOperationResult sent =
            run_async_http_operation(
                *request,
                WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,
                stop_event.get(),
                stop_token,
                deadline,
                [&request, &headers]() {
                    return WinHttpSendRequest(
                        request->get(),
                        headers,
                        static_cast<DWORD>(std::size(headers) - 1),
                        WINHTTP_NO_REQUEST_DATA,
                        0,
                        0,
                        request->context_value());
                });
        if (!sent.succeeded) {
            return report_async_http_failure(sent, error);
        }
        const AsyncHttpOperationResult received =
            run_async_http_operation(
                *request,
                WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,
                stop_event.get(),
                stop_token,
                deadline,
                [&request]() {
                    return WinHttpReceiveResponse(
                        request->get(), nullptr);
                });
        if (!received.succeeded) {
            return report_async_http_failure(received, error);
        }

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (!WinHttpQueryHeaders(request->get(),
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status,
                                 &status_size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            set_error(error, windows_error_message(GetLastError()));
            return false;
        }
        if (status == HTTP_STATUS_OK) {
            response = std::move(request);
            response_connection = std::move(connection);
            break;
        }
        if (!redirect_status(status)) {
            set_error(error, std::format(L"下载服务器返回 HTTP {}。", status));
            return false;
        }
        if (redirect_count == kMaximumRedirects) {
            set_error(error, L"更新下载重定向次数过多。");
            return false;
        }
        const auto location =
            redirect_location(request->get(), error);
        const auto combined =
            location ? combine_redirect_url(current_url, *location, error) : std::nullopt;
        if (!combined) {
            return false;
        }
        current_url = *combined;
    }
    if (!response) {
        set_error(error, L"更新下载未收到有效响应。");
        return false;
    }

    const auto content_length =
        response_content_length(response->get());
    if (content_length &&
        (*content_length == 0 || *content_length > maximum_size ||
         expected_size && *content_length != *expected_size)) {
        set_error(error, L"下载文件大小与允许范围或发布清单不一致。");
        return false;
    }

    if (!revalidate_directory_guard(directory_guard, error)) {
        return false;
    }
    UniqueFile created_destination(
        CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY |
                FILE_FLAG_SEQUENTIAL_SCAN |
                FILE_FLAG_WRITE_THROUGH,
            nullptr));
    if (!created_destination) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    ScopedPathCleanup cleanup(path);
    UniqueFile destination(created_destination.release());
    if (!revalidate_directory_guard(directory_guard, error)) {
        return false;
    }

    std::uint64_t total = 0;
    while (true) {
        if (cancellation_requested(stop_token, error)) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            set_error(error, L"更新下载超时。");
            return false;
        }
        const AsyncHttpOperationResult read_result =
            run_async_http_operation(
                *response,
                WINHTTP_CALLBACK_STATUS_READ_COMPLETE,
                stop_event.get(),
                stop_token,
                deadline,
                [&response]() {
                    const std::span<std::byte> buffer =
                        response->read_buffer();
                    return WinHttpReadData(
                        response->get(),
                        buffer.data(),
                        static_cast<DWORD>(buffer.size()),
                        nullptr);
                });
        if (!read_result.succeeded) {
            return report_async_http_failure(read_result, error);
        }
        const DWORD read = read_result.information_length;
        if (read == 0) {
            break;
        }
        const std::span<std::byte> buffer =
            response->read_buffer();
        if (read > buffer.size()) {
            set_error(
                error,
                L"下载回调返回了无效的数据长度。");
            return false;
        }
        if (total > maximum_size ||
            read > maximum_size - total ||
            (expected_size &&
             (total > *expected_size || read > *expected_size - total))) {
            set_error(error, L"下载文件超过允许的大小。");
            return false;
        }
        if (!write_all(
                destination.get(),
                std::span<const std::byte>(
                    buffer.data(), read),
                error)) {
            return false;
        }
        total += read;
    }
    if (total == 0 || expected_size && total != *expected_size) {
        set_error(error, L"下载文件大小与发布清单不一致。");
        return false;
    }
    if (cancellation_requested(stop_token, error)) {
        return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        set_error(error, L"更新下载超时。");
        return false;
    }
    if (!FlushFileBuffers(destination.get())) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    if (cancellation_requested(stop_token, error)) {
        return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        set_error(error, L"更新下载超时。");
        return false;
    }
    destination.reset();
    if (!revalidate_directory_guard(directory_guard, error)) {
        return false;
    }
    cleanup.release();
    return true;
}

std::wstring sha256_handle(HANDLE file, std::wstring* error) {
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        set_error(error, windows_error_message(GetLastError()));
        return {};
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        set_error(error, L"无法初始化 SHA256 校验。");
        if (hash) {
            BCryptDestroyHash(hash);
        }
        if (algorithm) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        return {};
    }

    std::wstring result;
    std::array<std::uint8_t, 8 * 1024> buffer{};
    DWORD read = 0;
    BOOL read_result = TRUE;
    bool ok = true;
    while ((read_result = ReadFile(
                file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) &&
           read > 0) {
        if (BCryptHashData(hash, buffer.data(), read, 0) != 0) {
            set_error(error, L"无法计算 SHA256 校验。");
            ok = false;
            break;
        }
    }
    if (!read_result) {
        set_error(error, windows_error_message(GetLastError()));
        ok = false;
    }
    if (ok) {
        std::array<std::uint8_t, 32> digest{};
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) ==
            0) {
            result.reserve(digest.size() * 2);
            for (const std::uint8_t byte : digest) {
                result += std::format(L"{:02X}", byte);
            }
        } else {
            set_error(error, L"无法完成 SHA256 校验。");
        }
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

std::optional<std::wstring> signer_sha256(const std::filesystem::path& path) {
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0;
    DWORD content_type = 0;
    DWORD format_type = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE,
                          path.c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY,
                          0,
                          &encoding,
                          &content_type,
                          &format_type,
                          &store,
                          &message,
                          nullptr)) {
        return std::nullopt;
    }

    DWORD signer_size = 0;
    if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signer_size)) {
        CryptMsgClose(message);
        CertCloseStore(store, 0);
        return std::nullopt;
    }
    std::vector<std::uint8_t> signer_bytes(signer_size);
    if (!CryptMsgGetParam(
            message, CMSG_SIGNER_INFO_PARAM, 0, signer_bytes.data(), &signer_size)) {
        CryptMsgClose(message);
        CertCloseStore(store, 0);
        return std::nullopt;
    }

    const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(signer_bytes.data());
    CERT_INFO certificate_info{};
    certificate_info.Issuer = signer->Issuer;
    certificate_info.SerialNumber = signer->SerialNumber;
    PCCERT_CONTEXT certificate = CertFindCertificateInStore(
        store, encoding, 0, CERT_FIND_SUBJECT_CERT, &certificate_info, nullptr);

    std::optional<std::wstring> result;
    if (certificate) {
        DWORD hash_size = 0;
        if (CertGetCertificateContextProperty(
                certificate, CERT_SHA256_HASH_PROP_ID, nullptr, &hash_size)) {
            std::vector<std::uint8_t> hash(hash_size);
            if (CertGetCertificateContextProperty(
                    certificate, CERT_SHA256_HASH_PROP_ID, hash.data(), &hash_size)) {
                std::wstring value;
                value.reserve(hash.size() * 2);
                for (const std::uint8_t byte : hash) {
                    value += std::format(L"{:02X}", byte);
                }
                result = std::move(value);
            }
        }
        CertFreeCertificateContext(certificate);
    }
    CryptMsgClose(message);
    CertCloseStore(store, 0);
    return result;
}

bool authenticode_integrity_is_valid(
    const std::filesystem::path& path,
    std::wstring* error) {
    WINTRUST_FILE_INFO file_info{sizeof(file_info)};
    file_info.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust_data{sizeof(trust_data)};
    trust_data.dwUIChoice = WTD_UI_NONE;
    trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust_data.dwUnionChoice = WTD_CHOICE_FILE;
    trust_data.pFile = &file_info;
    trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
    trust_data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &trust_data);
    trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trust_data);

    // The current release contract permits a pinned self-signed code-signing certificate.
    // Integrity failures return different status values and are never accepted here.
    if (status == ERROR_SUCCESS || status == CERT_E_UNTRUSTEDROOT ||
        status == CERT_E_CHAINING) {
        return true;
    }
    set_error(error,
              std::format(
                  L"更新程序 Authenticode 签名无效：0x{:08X}",
                  static_cast<unsigned int>(status)));
    return false;
}

std::optional<std::wstring> executable_version(const std::filesystem::path& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) {
        return std::nullopt;
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(bytes.data(),
                        L"\\",
                        reinterpret_cast<void**>(&info),
                        &info_size) ||
        !info || info_size < sizeof(VS_FIXEDFILEINFO) ||
        info->dwSignature != VS_FFI_SIGNATURE) {
        return std::nullopt;
    }
    return std::format(L"{}.{}.{}",
                       HIWORD(info->dwProductVersionMS),
                       LOWORD(info->dwProductVersionMS),
                       HIWORD(info->dwProductVersionLS));
}

bool target_directory_is_writable(
    const std::filesystem::path& target,
    std::wstring* error) {
    UniqueFile target_file = open_regular_file(target, FILE_SHARE_READ, error);
    if (!target_file) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            target_file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        set_error(
            error,
            L"当前 AirScreenshot.exe 是只读文件，请取消只读属性或移到普通文件夹后再运行。");
        return false;
    }

    const std::filesystem::path directory = target.parent_path();
    if (directory.empty() || !is_safe_directory_tree(directory)) {
        set_error(error, L"当前程序目录不可访问或是 reparse point。");
        return false;
    }
    const auto probe = unique_path(directory, L"write-test", L".tmp");
    if (!probe) {
        set_error(error, L"无法创建写入探针。");
        return false;
    }
    UniqueFile file(CreateFileW(probe->c_str(),
                                GENERIC_WRITE,
                                0,
                                nullptr,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                nullptr));
    if (!file) {
        set_error(
            error,
            L"当前目录不可写，请将 AirScreenshot.exe 移到普通文件夹后再运行。");
        return false;
    }
    return true;
}

bool target_directory_is_writable(std::wstring* error) {
    const auto executable = portable_executable_path();
    return !executable.empty() && target_directory_is_writable(executable, error);
}

bool create_process(
    const std::filesystem::path& executable,
    std::wstring command,
    std::wstring* error,
    UniqueFile* process_handle = nullptr) {
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(),
                        command.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        executable.parent_path().c_str(),
                        &startup,
                        &process)) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    CloseHandle(process.hThread);
    if (process_handle) {
        process_handle->reset(process.hProcess);
    } else {
        CloseHandle(process.hProcess);
    }
    return true;
}

std::optional<DWORD> parse_process_id(std::wstring_view value) {
    if (value.empty() || (value.size() > 1 && value.front() == L'0')) {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        const unsigned int digit = static_cast<unsigned int>(character - L'0');
        if (result > (std::numeric_limits<DWORD>::max() - digit) / 10ULL) {
            return std::nullopt;
        }
        result = result * 10ULL + digit;
    }
    if (result == 0 || result == GetCurrentProcessId()) {
        return std::nullopt;
    }
    return static_cast<DWORD>(result);
}

std::optional<DWORD> current_parent_process_id() {
    UniqueFile snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return std::nullopt;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return std::nullopt;
    }
    do {
        if (entry.th32ProcessID == GetCurrentProcessId()) {
            return entry.th32ParentProcessID;
        }
    } while (Process32NextW(snapshot.get(), &entry));
    return std::nullopt;
}

std::optional<std::filesystem::path> process_image_path(HANDLE process) {
    std::wstring value(32'768, L'\0');
    DWORD size = static_cast<DWORD>(value.size());
    if (!QueryFullProcessImageNameW(process, 0, value.data(), &size) || size == 0) {
        return std::nullopt;
    }
    value.resize(size);
    return std::filesystem::path(value);
}

bool process_started_no_later_than(
    HANDLE candidate,
    HANDLE reference) {
    FILETIME candidate_created{};
    FILETIME candidate_exited{};
    FILETIME candidate_kernel{};
    FILETIME candidate_user{};
    FILETIME reference_created{};
    FILETIME reference_exited{};
    FILETIME reference_kernel{};
    FILETIME reference_user{};
    if (!GetProcessTimes(
            candidate,
            &candidate_created,
            &candidate_exited,
            &candidate_kernel,
            &candidate_user) ||
        !GetProcessTimes(
            reference,
            &reference_created,
            &reference_exited,
            &reference_kernel,
            &reference_user)) {
        return false;
    }
    const ULARGE_INTEGER candidate_time{
        candidate_created.dwLowDateTime,
        candidate_created.dwHighDateTime};
    const ULARGE_INTEGER reference_time{
        reference_created.dwLowDateTime,
        reference_created.dwHighDateTime};
    return candidate_time.QuadPart <= reference_time.QuadPart;
}

bool verify_existing_install(
    const std::filesystem::path& path,
    std::wstring_view candidate_version,
    FileIdentity* identity,
    UniqueFile* stable_guard,
    std::wstring* error) {
    UniqueFile guard = open_regular_file(path, FILE_SHARE_READ, error);
    if (!guard) {
        return false;
    }
    const auto current_identity = file_identity(guard.get());
    if (!current_identity || !authenticode_integrity_is_valid(path, error)) {
        return false;
    }
    const auto signer = signer_sha256(path);
    if (!signer || *signer != normalized_hex(kReleaseSignerSha256)) {
        set_error(error, L"待替换程序不是受信任的 AirScreenshot 发布版本。");
        return false;
    }
    const auto version = executable_version(path);
    if (!version || !version_is_newer(*version, candidate_version)) {
        set_error(error, L"更新 helper 只能替换同一发布者的较旧 AirScreenshot 版本。");
        return false;
    }
    if (identity) {
        *identity = *current_identity;
    }
    if (stable_guard) {
        *stable_guard = std::move(guard);
    }
    return true;
}

bool helper_matches_pending_update(
    const std::filesystem::path& directory,
    UpdateManifest* manifest,
    std::wstring* error) {
    const auto text = read_text_file(pending_manifest_path(directory), error);
    const auto parsed = text ? parse_pending_update(*text) : std::nullopt;
    if (!parsed) {
        set_error(error, L"找不到有效的待处理更新清单。");
        return false;
    }
    const std::filesystem::path self = portable_executable_path();
    const std::filesystem::path expected =
        update_executable_path(directory, parsed->manifest.version);
    if (self.empty() || !same_file(self, expected, error) ||
        !verify_portable_executable(self, parsed->manifest, error)) {
        if (error && error->empty()) {
            *error = L"更新 helper 与待处理更新不匹配。";
        }
        return false;
    }
    if (manifest) {
        *manifest = parsed->manifest;
    }
    return true;
}

bool helper_is_trusted_legacy_update(
    const std::filesystem::path& directory,
    UpdateManifest* manifest,
    std::wstring* error) {
    clear_error(error);
    const std::filesystem::path self = portable_executable_path();
    const std::wstring filename = self.filename().wstring();
    constexpr std::wstring_view prefix = L"AirScreenshot-";
    constexpr std::wstring_view extension = L".exe";
    if (self.empty() || !filename.starts_with(prefix) ||
        !filename.ends_with(extension) ||
        filename.size() <= prefix.size() + extension.size()) {
        set_error(error, L"旧版更新 helper 的文件名无效。");
        return false;
    }
    const std::wstring version = filename.substr(
        prefix.size(),
        filename.size() - prefix.size() - extension.size());
    if (!valid_version(version) ||
        !same_file(
            self, update_executable_path(directory, version), error)) {
        set_error(error, L"旧版更新 helper 不在受控更新目录。");
        return false;
    }

    UniqueFile guard = open_regular_file(self, FILE_SHARE_READ, error);
    const auto size = guard ? file_size(guard.get()) : std::nullopt;
    if (!size || *size == 0 || *size > kMaxPortableUpdateBytes) {
        set_error(error, L"旧版更新 helper 的大小无效。");
        return false;
    }
    const std::wstring hash = sha256_handle(guard.get(), error);
    if (hash.size() != 64 ||
        !authenticode_integrity_is_valid(self, error)) {
        return false;
    }
    const auto signer = signer_sha256(self);
    const auto resource_version = executable_version(self);
    if (!signer || *signer != normalized_hex(kReleaseSignerSha256) ||
        !resource_version || *resource_version != version) {
        set_error(error, L"旧版更新 helper 的发布者或版本无效。");
        return false;
    }

    if (manifest) {
        manifest->version = version;
        manifest->url =
            L"https://mmm1h.github.io/air-screenshot/AirScreenshot.exe";
        manifest->sha256 = hash;
        manifest->size = *size;
    }
    return true;
}

bool copy_and_flush_regular_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const DirectoryTreeGuard& directory_guard,
    DWORD additional_access,
    UniqueFile* destination_guard,
    std::wstring* error) {
    if (!path_matches_directory_guard(
            destination.parent_path(), directory_guard) ||
        !revalidate_directory_guard(directory_guard, error) ||
        !safe_existing_destination(destination, error)) {
        if (error && error->empty()) {
            *error = L"原子更新临时文件不在受控目录中。";
        }
        return false;
    }
    if (!CopyFileW(source.c_str(), destination.c_str(), TRUE)) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }
    if (!revalidate_directory_guard(directory_guard, error)) {
        return false;
    }
    UniqueFile copied = open_regular_file_with_access(
        destination,
        GENERIC_READ | GENERIC_WRITE | READ_CONTROL |
            additional_access,
        FILE_SHARE_READ,
        error);
    if (!copied || !FlushFileBuffers(copied.get())) {
        if (error && error->empty()) {
            *error = windows_error_message(GetLastError());
        }
        return false;
    }
    if (!revalidate_directory_guard(directory_guard, error)) {
        return false;
    }
    if (destination_guard) {
        *destination_guard = std::move(copied);
    }
    return true;
}

bool reopen_flushed_file_for_verification(
    const std::filesystem::path& path,
    const DirectoryTreeGuard& directory_guard,
    UniqueFile* file_guard,
    std::wstring* error) {
    if (!file_guard || !*file_guard) {
        set_error(error, L"刷盘文件 guard 无效。");
        return false;
    }
    const auto expected_identity =
        file_identity(file_guard->get());
    if (!expected_identity) {
        set_error(error, L"无法读取刷盘文件标识。");
        return false;
    }

    file_guard->reset();
    if (!revalidate_directory_guard(directory_guard, error)) {
        return false;
    }
    UniqueFile reopened = open_regular_file_with_access(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        error,
        kSecurityReopenAttempts);
    const auto reopened_identity =
        reopened ? file_identity(reopened.get()) : std::nullopt;
    if (!reopened_identity ||
        *reopened_identity != *expected_identity) {
        if (error && error->empty()) {
            *error = L"刷盘文件在只读重开期间发生了变化。";
        }
        return false;
    }
    if (!revalidate_directory_guard(directory_guard, error)) {
        return false;
    }
    *file_guard = std::move(reopened);
    return true;
}

bool replace_file_at_path(
    const std::filesystem::path& target,
    const std::filesystem::path& replacement,
    bool persistent_acls,
    const DirectoryTreeGuard& directory_guard,
    std::wstring* error,
    bool force_post_commit_validation_failure = false) {
    DWORD replace_error = ERROR_GEN_FAILURE;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (!revalidate_directory_guard(directory_guard, error)) {
            return false;
        }
        if (ReplaceFileW(
                target.c_str(),
                replacement.c_str(),
                nullptr,
                0,
                nullptr,
                nullptr)) {
            if (force_post_commit_validation_failure) {
                set_error(
                    error,
                    L"测试注入：文件替换已提交，但后置目录校验失败。");
                return false;
            }
            return revalidate_directory_guard(
                directory_guard, error);
        }
        replace_error = GetLastError();
        if (replace_error != ERROR_SHARING_VIOLATION &&
            replace_error != ERROR_LOCK_VIOLATION &&
            replace_error != ERROR_ACCESS_DENIED) {
            break;
        }
        Sleep(50);
    }
    if (persistent_acls) {
        set_error(error, windows_error_message(replace_error));
        return false;
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
        if (!revalidate_directory_guard(directory_guard, error)) {
            return false;
        }
        if (MoveFileExW(
                replacement.c_str(),
                target.c_str(),
                MOVEFILE_REPLACE_EXISTING |
                    MOVEFILE_WRITE_THROUGH)) {
            if (force_post_commit_validation_failure) {
                set_error(
                    error,
                    L"测试注入：文件移动已提交，但后置目录校验失败。");
                return false;
            }
            return revalidate_directory_guard(
                directory_guard, error);
        }
        replace_error = GetLastError();
        if (replace_error != ERROR_SHARING_VIOLATION &&
            replace_error != ERROR_LOCK_VIOLATION &&
            replace_error != ERROR_ACCESS_DENIED) {
            break;
        }
        Sleep(50);
    }
    set_error(error, windows_error_message(replace_error));
    return false;
}

bool target_matches_original(
    const std::filesystem::path& target,
    const FileIdentity& expected_identity,
    std::wstring_view expected_hash,
    const std::optional<SecurityDescriptorSnapshot>& expected_security) {
    std::wstring ignored;
    UniqueFile target_guard =
        open_regular_file(target, FILE_SHARE_READ, &ignored);
    const auto identity =
        target_guard ? file_identity(target_guard.get())
                     : std::nullopt;
    if (!identity || *identity != expected_identity ||
        sha256_handle(target_guard.get(), &ignored) !=
            expected_hash) {
        return false;
    }
    return !expected_security ||
           verify_security_descriptor(
               target_guard.get(), *expected_security, nullptr);
}

bool target_matches_update(
    const std::filesystem::path& target,
    const UpdateManifest& manifest,
    const std::optional<SecurityDescriptorSnapshot>& expected_security) {
    std::wstring ignored;
    UniqueFile target_guard =
        open_regular_file(target, FILE_SHARE_READ, &ignored);
    return target_guard &&
           verify_portable_executable(
               target, manifest, &ignored) &&
           (!expected_security ||
            verify_security_descriptor(
                target_guard.get(),
                *expected_security,
                &ignored));
}

bool restore_backup_at_path(
    const std::filesystem::path& target,
    const std::filesystem::path& backup,
    bool persistent_acls,
    const DirectoryTreeGuard& directory_guard,
    std::wstring* error,
    bool force_post_commit_validation_failure = false) {
    if (path_is_missing(target)) {
        if (!revalidate_directory_guard(
                directory_guard, error) ||
            !MoveFileExW(
                backup.c_str(),
                target.c_str(),
                MOVEFILE_WRITE_THROUGH)) {
            if (error && error->empty()) {
                *error = windows_error_message(GetLastError());
            }
            return false;
        }
        if (force_post_commit_validation_failure) {
            set_error(
                error,
                L"测试注入：备份恢复已提交，但后置目录校验失败。");
            return false;
        }
        return revalidate_directory_guard(
            directory_guard, error);
    }
    return replace_file_at_path(
        target,
        backup,
        persistent_acls,
        directory_guard,
        error,
        force_post_commit_validation_failure);
}

bool verify_restored_target(
    const std::filesystem::path& target,
    std::wstring_view expected_hash,
    const std::optional<SecurityDescriptorSnapshot>& expected_security,
    bool allow_security_repair,
    std::wstring* error) {
    const DWORD security_access =
        allow_security_repair && expected_security
            ? GENERIC_WRITE | WRITE_DAC | WRITE_OWNER
            : 0;
    UniqueFile restored =
        open_regular_file_with_access(
            target,
            GENERIC_READ | READ_CONTROL | security_access,
            FILE_SHARE_READ,
            error);
    if (!restored ||
        sha256_handle(restored.get(), error) != expected_hash) {
        if (error && error->empty()) {
            *error = L"恢复后的程序内容与原版本不一致。";
        }
        return false;
    }
    if (!expected_security) {
        return true;
    }
    if (!verify_security_descriptor(
            restored.get(), *expected_security, nullptr)) {
        if (!allow_security_repair ||
            !apply_security_descriptor(
                restored.get(), *expected_security, error) ||
            !FlushFileBuffers(restored.get())) {
            if (error && error->empty()) {
                *error = L"恢复后的程序安全描述符不一致。";
            }
            return false;
        }
    }
    return verify_security_descriptor(
        restored.get(), *expected_security, error);
}

bool backup_matches_original(
    const std::filesystem::path& backup,
    std::wstring_view expected_hash,
    const std::optional<SecurityDescriptorSnapshot>& expected_security) {
    std::wstring ignored;
    UniqueFile guard =
        open_regular_file(backup, FILE_SHARE_READ, &ignored);
    return guard &&
           sha256_handle(guard.get(), &ignored) == expected_hash &&
           (!expected_security ||
            verify_security_descriptor(
                guard.get(), *expected_security, &ignored));
}

struct RollbackOutcome {
    bool restored{};
    bool backup_retained{};
    std::wstring detail;
};

RollbackOutcome restore_and_verify_backup(
    const std::filesystem::path& target,
    const std::filesystem::path& backup,
    bool persistent_acls,
    const DirectoryTreeGuard& directory_guard,
    std::wstring_view expected_hash,
    const std::optional<SecurityDescriptorSnapshot>& expected_security,
    bool force_post_commit_validation_failure = false) {
    std::wstring operation_error;
    const bool restore_reported_success =
        restore_backup_at_path(
            target,
            backup,
            persistent_acls,
            directory_guard,
            &operation_error,
            force_post_commit_validation_failure);
    const bool directory_stable =
        revalidate_directory_guard(directory_guard, nullptr);

    std::wstring verification_error;
    if (verify_restored_target(
            target,
            expected_hash,
            expected_security,
            directory_stable,
            &verification_error)) {
        RollbackOutcome outcome;
        outcome.restored = true;
        if (!restore_reported_success || !directory_stable) {
            outcome.detail =
                operation_error.empty()
                    ? L"恢复 API 的后置目录校验未通过，但旧版本已按内容和安全描述符核验。"
                    : operation_error;
        }
        return outcome;
    }

    RollbackOutcome outcome;
    outcome.backup_retained =
        directory_stable &&
        backup_matches_original(
            backup, expected_hash, expected_security);
    if (!operation_error.empty()) {
        outcome.detail = operation_error;
    }
    if (!verification_error.empty()) {
        if (!outcome.detail.empty()) {
            outcome.detail += L"；";
        }
        outcome.detail += verification_error;
    }
    if (outcome.detail.empty()) {
        outcome.detail = L"无法确认旧版本的恢复状态。";
    }
    return outcome;
}

bool atomic_replace_target(
    const std::filesystem::path& target,
    const UpdateManifest& manifest,
    const FileIdentity& expected_identity,
    UniqueFile* verified_target,
    StableTargetGuard* stable_target,
    std::wstring* error) {
    const std::filesystem::path source = portable_executable_path();
    auto directory_guard =
        lock_safe_directory_tree(target.parent_path(), error);
    if (!directory_guard) {
        return false;
    }
    if (!verified_target || !*verified_target) {
        set_error(error, L"更新目标 guard 无效。");
        return false;
    }
    const auto current_identity =
        file_identity(verified_target->get());
    if (!current_identity || *current_identity != expected_identity) {
        set_error(error, L"更新目标在替换前发生了变化。");
        return false;
    }
    const std::wstring original_hash =
        sha256_handle(verified_target->get(), error);
    if (original_hash.size() != 64) {
        return false;
    }
    const auto persistent_acls =
        volume_supports_persistent_acls(target, error);
    if (!persistent_acls) {
        return false;
    }
    std::optional<SecurityDescriptorSnapshot> original_security;
    if (*persistent_acls) {
        original_security =
            capture_security_descriptor(
                verified_target->get(), error);
        if (!original_security) {
            return false;
        }
    }

    const auto replacement = unique_path(target.parent_path(), L"replacement", L".tmp");
    const auto backup = unique_path(target.parent_path(), L"backup", L".tmp");
    if (!replacement || !backup) {
        set_error(error, L"无法创建原子更新临时文件名。");
        return false;
    }
    ScopedPathCleanup replacement_cleanup(*replacement);
    ScopedPathCleanup backup_cleanup(*backup);
    UniqueFile replacement_guard;
    if (!copy_and_flush_regular_file(
            source,
            *replacement,
            *directory_guard,
            0,
            &replacement_guard,
            error)) {
        return false;
    }
    if (!reopen_flushed_file_for_verification(
            *replacement,
            *directory_guard,
            &replacement_guard,
            error) ||
        !verify_portable_executable(*replacement, manifest, error)) {
        return false;
    }

    UniqueFile backup_guard;
    const DWORD backup_security_access =
        original_security ? WRITE_DAC | WRITE_OWNER : 0;
    if (!copy_and_flush_regular_file(
            target,
            *backup,
            *directory_guard,
            backup_security_access,
            &backup_guard,
            error)) {
        return false;
    }
    if (!backup_guard ||
        sha256_handle(backup_guard.get(), error) != original_hash) {
        if (error && error->empty()) {
            *error = L"更新回滚备份与原程序不一致。";
        }
        return false;
    }
    if (original_security &&
        (!apply_security_descriptor(
             backup_guard.get(), *original_security, error) ||
         !FlushFileBuffers(backup_guard.get()) ||
         !verify_security_descriptor(
             backup_guard.get(), *original_security, error))) {
        if (error && error->empty()) {
            *error = L"无法准备保留原安全描述符的回滚备份。";
        }
        return false;
    }

    UniqueFile path_target =
        open_regular_file(target, FILE_SHARE_READ, error);
    const auto path_identity =
        path_target ? file_identity(path_target.get())
                    : std::nullopt;
    if (!path_identity || *path_identity != expected_identity) {
        set_error(error, L"更新目标在替换前发生了变化。");
        return false;
    }
    path_target.reset();
    replacement_guard.reset();
    backup_guard.reset();
    verified_target->reset();
    const bool switch_reported_success =
        replace_file_at_path(
            target,
            *replacement,
            *persistent_acls,
            *directory_guard,
            error);
    if (!switch_reported_success) {
        const std::wstring switch_error =
            error && !error->empty()
                ? *error
                : L"无法切换更新程序。";
        if (target_matches_original(
                target,
                expected_identity,
                original_hash,
                original_security)) {
            set_error(
                error,
                switch_error +
                    L"；原程序保持不变。");
            return false;
        }
        if (!target_matches_update(
                target, manifest, original_security)) {
            const RollbackOutcome rollback =
                restore_and_verify_backup(
                    target,
                    *backup,
                    *persistent_acls,
                    *directory_guard,
                    original_hash,
                    original_security);
            if (!rollback.restored) {
                std::wstring message = std::format(
                    L"{}；切换结果不完整且恢复未通过核验：{}",
                    switch_error,
                    rollback.detail);
                if (rollback.backup_retained) {
                    message += std::format(
                        L"。经核验的回滚备份保留在：{}",
                        backup->wstring());
                } else {
                    message += L"；无法确认可用回滚备份。";
                }
                if (rollback.backup_retained) {
                    backup_cleanup.release();
                }
                set_error(error, std::move(message));
                return false;
            }
            if (!rollback.detail.empty()) {
                set_error(
                    error,
                    std::format(
                        L"{}；已恢复并核验旧版本，但恢复 API 的后置状态异常：{}",
                        switch_error,
                        rollback.detail));
                return false;
            }
            set_error(
                error,
                switch_error +
                    L"；已恢复旧版本。");
            return false;
        }
    } else {
        replacement_cleanup.release();
    }

    UniqueFile installed_guard =
        open_regular_file(target, FILE_SHARE_READ, error);
    const bool installed_content_valid =
        installed_guard &&
        verify_portable_executable(target, manifest, error);
    const bool installed_security_valid =
        installed_content_valid &&
        (!original_security ||
         ensure_security_descriptor(
             target,
             *original_security,
             &installed_guard,
             error));
    const bool installed_valid =
        installed_security_valid &&
        verify_portable_executable(
            target, manifest, error);
    if (!installed_valid) {
        const std::wstring install_error =
            error && !error->empty() ? *error : L"更新后程序校验失败。";
        installed_guard.reset();
        const RollbackOutcome rollback =
            restore_and_verify_backup(
                target,
                *backup,
                *persistent_acls,
                *directory_guard,
                original_hash,
                original_security);
        if (!rollback.restored) {
            std::wstring message = std::format(
                L"{}；恢复旧版本未通过核验：{}",
                install_error,
                rollback.detail);
            if (rollback.backup_retained) {
                message += std::format(
                    L"。经核验的回滚备份保留在：{}",
                    backup->wstring());
                backup_cleanup.release();
            } else {
                message += L"；无法确认可用回滚备份。";
            }
            set_error(error, std::move(message));
            return false;
        }
        if (!rollback.detail.empty()) {
            set_error(
                error,
                std::format(
                    L"{}；已恢复并核验旧版本，但恢复 API 的后置状态异常：{}",
                    install_error,
                    rollback.detail));
            return false;
        }
        set_error(
            error,
            install_error + L"；已恢复并核验旧版本。");
        return false;
    }

    if (DeleteFileW(backup->c_str())) {
        backup_cleanup.release();
    }
    if (stable_target) {
        stable_target->directories = std::move(*directory_guard);
        stable_target->target = std::move(installed_guard);
    }
    return true;
}

bool staged_filename(std::wstring_view name) {
    constexpr std::wstring_view prefix = L"AirScreenshot-";
    constexpr std::wstring_view extension = L".exe";
    if (!name.starts_with(prefix) || !name.ends_with(extension) ||
        name.size() <= prefix.size() + extension.size()) {
        return false;
    }
    return valid_version(
        name.substr(prefix.size(), name.size() - prefix.size() - extension.size()));
}

bool temporary_update_filename(std::wstring_view name) {
    return name.starts_with(L".airshot-") &&
           (name.ends_with(L".tmp") || name.ends_with(L".download"));
}

bool remove_regular_file(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ||
               GetLastError() == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return false;
    }
    return DeleteFileW(path.c_str()) != FALSE;
}

std::optional<PendingUpdate> validated_pending_update_unlocked(
    const std::filesystem::path& directory) {
    const auto text = read_text_file(pending_manifest_path(directory));
    const auto pending = text ? parse_pending_update(*text) : std::nullopt;
    if (!pending ||
        !version_is_newer(
            from_utf8(AIRSHOT_VERSION), pending->manifest.version)) {
        return std::nullopt;
    }
    if (!verify_portable_executable(
            update_executable_path(directory, pending->manifest.version),
            pending->manifest)) {
        return std::nullopt;
    }
    return pending;
}

bool pending_update_available_unlocked(
    const std::filesystem::path& directory,
    bool allow_automatic_pending = true) {
    const auto pending = validated_pending_update_unlocked(directory);
    return pending &&
           portable_internal::pending_update_source_allowed(
               pending->source, allow_automatic_pending);
}

}  // namespace

namespace portable_internal {

std::optional<UpdateRequestSource> pending_update_source_from_json(
    std::wstring_view json) {
    const auto pending = parse_pending_update(json);
    return pending
               ? std::optional<UpdateRequestSource>(pending->source)
               : std::nullopt;
}

std::wstring pending_update_to_json_for_testing(
    const UpdateManifest& manifest,
    UpdateRequestSource source) {
    return pending_update_to_json(PendingUpdate{manifest, source});
}

bool pending_update_source_allowed(
    UpdateRequestSource source,
    bool allow_automatic_pending) noexcept {
    return allow_automatic_pending ||
           source == UpdateRequestSource::manual;
}

bool named_object_security_uses_current_user_owner(
    std::wstring* error) {
    clear_error(error);
    auto security =
        current_user_named_object_security(
            MUTEX_ALL_ACCESS, error);
    if (!security) {
        return false;
    }
    PSID descriptor_owner = nullptr;
    BOOL owner_defaulted = TRUE;
    if (!GetSecurityDescriptorOwner(
            &security->descriptor,
            &descriptor_owner,
            &owner_defaulted) ||
        !descriptor_owner || owner_defaulted ||
        !EqualSid(
            descriptor_owner,
            security->user_sid.data())) {
        set_error(
            error,
            L"命名对象安全描述符未显式指定当前用户 owner。");
        return false;
    }
    SECURITY_ATTRIBUTES attributes{
        sizeof(attributes),
        &security->descriptor,
        FALSE};
    UniqueFile mutex(
        CreateMutexW(&attributes, FALSE, nullptr));
    if (!mutex) {
        set_error(error, windows_error_message(GetLastError()));
        return false;
    }

    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD security_error = GetSecurityInfo(
        mutex.get(),
        SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION,
        &owner,
        nullptr,
        nullptr,
        nullptr,
        &descriptor);
    const bool expected_owner =
        security_error == ERROR_SUCCESS && owner &&
        EqualSid(owner, security->user_sid.data());
    if (descriptor) {
        LocalFree(descriptor);
    }
    if (!expected_owner) {
        set_error(
            error,
            security_error == ERROR_SUCCESS
                ? L"命名对象 owner 不是当前用户。"
                : windows_error_message(security_error));
    }
    return expected_owner;
}

bool security_descriptor_comparison_checks_sacl_state(
    std::wstring* error) {
    clear_error(error);
    auto security =
        current_user_named_object_security(
            MUTEX_ALL_ACCESS, error);
    if (!security) {
        return false;
    }
    const auto make_snapshot =
        [&security, error](
            BOOL sacl_present,
            bool sacl_protected)
            -> std::optional<SecurityDescriptorSnapshot> {
            SecurityDescriptorSnapshot snapshot;
            snapshot.bytes.resize(
                sizeof(SECURITY_DESCRIPTOR));
            auto* descriptor = snapshot.get();
            if (!InitializeSecurityDescriptor(
                    descriptor,
                    SECURITY_DESCRIPTOR_REVISION) ||
                !SetSecurityDescriptorOwner(
                    descriptor,
                    security->user_sid.data(),
                    FALSE) ||
                !SetSecurityDescriptorGroup(
                    descriptor,
                    security->user_sid.data(),
                    FALSE) ||
                !SetSecurityDescriptorDacl(
                    descriptor,
                    FALSE,
                    nullptr,
                    FALSE) ||
                !SetSecurityDescriptorSacl(
                    descriptor,
                    sacl_present,
                    nullptr,
                    FALSE) ||
                (sacl_protected &&
                 !SetSecurityDescriptorControl(
                     descriptor,
                     SE_SACL_PROTECTED,
                     SE_SACL_PROTECTED))) {
                set_error(
                    error,
                    windows_error_message(GetLastError()));
                return std::nullopt;
            }
            return snapshot;
        };

    const auto absent = make_snapshot(FALSE, false);
    const auto present = make_snapshot(TRUE, false);
    const auto protected_sacl = make_snapshot(TRUE, true);
    if (!absent || !present || !protected_sacl) {
        return false;
    }
    std::wstring presence_difference;
    std::wstring protection_difference;
    if (!security_descriptors_equivalent(
            *absent, *present, &presence_difference) ||
        security_descriptors_equivalent(
            *present,
            *protected_sacl,
            &protection_difference) ||
        !presence_difference.empty() ||
        protection_difference.empty()) {
        set_error(
            error,
            L"安全描述符比较器未正确规范 null label 或识别 SACL protection 差异。");
        return false;
    }
    return true;
}

bool rollback_post_commit_failure_is_reconciled(
    const std::filesystem::path& target,
    const std::filesystem::path& backup,
    std::wstring* error) {
    clear_error(error);
    try {
        auto directory_guard =
            lock_safe_directory_tree(
                target.parent_path(), error);
        if (!directory_guard ||
            !path_matches_directory_guard(
                backup.parent_path(), *directory_guard)) {
            if (error && error->empty()) {
                *error =
                    L"测试回滚文件不在同一受控目录中。";
            }
            return false;
        }
        const auto persistent_acls =
            volume_supports_persistent_acls(backup, error);
        UniqueFile backup_guard =
            open_regular_file(backup, FILE_SHARE_READ, error);
        const std::wstring expected_hash =
            backup_guard
                ? sha256_handle(backup_guard.get(), error)
                : std::wstring{};
        std::optional<SecurityDescriptorSnapshot>
            expected_security;
        if (persistent_acls && *persistent_acls &&
            backup_guard) {
            expected_security =
                capture_security_descriptor(
                    backup_guard.get(), error);
        }
        if (!persistent_acls || !backup_guard ||
            expected_hash.size() != 64 ||
            (*persistent_acls && !expected_security)) {
            if (error && error->empty()) {
                *error = L"无法捕获测试回滚备份状态。";
            }
            return false;
        }
        backup_guard.reset();

        const RollbackOutcome outcome =
            restore_and_verify_backup(
                target,
                backup,
                *persistent_acls,
                *directory_guard,
                expected_hash,
                expected_security,
                true);
        if (!outcome.restored ||
            outcome.backup_retained ||
            !path_is_missing(backup)) {
            set_error(
                error,
                outcome.detail.empty()
                    ? L"已提交的测试回滚未被正确收敛。"
                    : outcome.detail);
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return false;
    }
}

bool same_file_security(
    const std::filesystem::path& first,
    const std::filesystem::path& second,
    std::wstring* error) {
    clear_error(error);
    UniqueFile first_guard =
        open_regular_file(first, FILE_SHARE_READ, error);
    UniqueFile second_guard =
        open_regular_file(second, FILE_SHARE_READ, error);
    if (!first_guard || !second_guard) {
        return false;
    }
    const auto first_security =
        capture_security_descriptor(first_guard.get(), error);
    const auto second_security =
        capture_security_descriptor(second_guard.get(), error);
    if (!first_security || !second_security ||
        !security_descriptors_equivalent(
            *first_security, *second_security)) {
        if (error && error->empty()) {
            *error = L"文件安全描述符不一致。";
        }
        return false;
    }
    return true;
}

bool apply_file_security_from_reference(
    const std::filesystem::path& reference,
    const std::filesystem::path& destination,
    std::wstring* error) {
    clear_error(error);
    UniqueFile reference_guard =
        open_regular_file(
            reference, FILE_SHARE_READ, error);
    const auto expected =
        reference_guard
            ? capture_security_descriptor(
                  reference_guard.get(), error)
            : std::nullopt;
    if (!expected) {
        return false;
    }
    UniqueFile destination_guard =
        open_regular_file(
            destination, FILE_SHARE_READ, error);
    if (!destination_guard ||
        !ensure_security_descriptor(
            destination,
            *expected,
            &destination_guard,
            error) ||
        !open_regular_file(
            destination, FILE_SHARE_READ, error)) {
        if (error && error->empty()) {
            *error =
                L"测试文件安全描述符重放或独立只读校验失败。";
        }
        return false;
    }
    return true;
}

bool replace_file_preserving_security(
    const std::filesystem::path& target,
    const std::filesystem::path& replacement,
    const std::filesystem::path& backup,
    std::wstring* error) {
    clear_error(error);
    try {
        auto directory_guard =
            lock_safe_directory_tree(
                target.parent_path(), error);
        if (!directory_guard ||
            !path_matches_directory_guard(
                replacement.parent_path(),
                *directory_guard) ||
            !path_matches_directory_guard(
                backup.parent_path(),
                *directory_guard)) {
            if (error && error->empty()) {
                *error = L"测试替换文件不在同一受控目录中。";
            }
            return false;
        }
        const auto persistent_acls =
            volume_supports_persistent_acls(target, error);
        if (!persistent_acls) {
            if (error && error->empty()) {
                *error = L"无法查询测试卷的 ACL 能力。";
            }
            return false;
        }

        UniqueFile target_guard =
            open_regular_file(target, FILE_SHARE_READ, error);
        const auto identity =
            target_guard
                ? file_identity(target_guard.get())
                : std::nullopt;
        const std::wstring original_hash =
            target_guard
                ? sha256_handle(target_guard.get(), error)
                : std::wstring{};
        std::optional<SecurityDescriptorSnapshot>
            original_security;
        if (*persistent_acls && target_guard) {
            original_security =
                capture_security_descriptor(
                    target_guard.get(), error);
        }
        if (!target_guard || !identity ||
            original_hash.size() != 64 ||
            (*persistent_acls && !original_security)) {
            if (error && error->empty()) {
                *error = L"无法捕获测试目标的稳定状态。";
            }
            return false;
        }

        UniqueFile replacement_guard =
            open_regular_file_with_access(
                replacement,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ,
                error);
        if (!replacement_guard ||
            !FlushFileBuffers(replacement_guard.get()) ||
            !revalidate_directory_guard(
                *directory_guard, error)) {
            if (error && error->empty()) {
                *error = windows_error_message(GetLastError());
            }
            return false;
        }
        if (!reopen_flushed_file_for_verification(
                replacement,
                *directory_guard,
                &replacement_guard,
                error)) {
            return false;
        }
        {
            UniqueFile independent_verifier =
                open_regular_file(
                    replacement,
                    FILE_SHARE_READ,
                    error);
            if (!independent_verifier) {
                if (error && error->empty()) {
                    *error =
                        L"测试 replacement 无法执行独立只读校验。";
                }
                return false;
            }
        }

        UniqueFile backup_guard;
        if (!copy_and_flush_regular_file(
                target,
                backup,
                *directory_guard,
                original_security
                    ? WRITE_DAC | WRITE_OWNER
                    : 0,
                &backup_guard,
                error)) {
            if (error && error->empty()) {
                *error = L"无法复制或刷新测试回滚备份。";
            }
            return false;
        }
        if (original_security) {
            if (!apply_security_descriptor(
                    backup_guard.get(),
                    *original_security,
                    error)) {
                if (error && error->empty()) {
                    *error = L"无法应用测试回滚备份的安全描述符。";
                }
                return false;
            }
            if (!FlushFileBuffers(backup_guard.get())) {
                set_error(
                    error,
                    std::format(
                        L"刷新测试回滚备份失败：{}",
                        windows_error_message(GetLastError())));
                return false;
            }
            if (!verify_security_descriptor(
                    backup_guard.get(),
                    *original_security,
                    error)) {
                if (error && error->empty()) {
                    *error = L"测试回滚备份的安全描述符校验失败。";
                }
                return false;
            }
        }

        target_guard.reset();
        replacement_guard.reset();
        backup_guard.reset();
        if (!replace_file_at_path(
                target,
                replacement,
                *persistent_acls,
                *directory_guard,
                error)) {
            if (target_matches_original(
                    target,
                    *identity,
                    original_hash,
                    original_security)) {
                if (error && error->empty()) {
                    *error = L"测试替换未切换目标。";
                }
                return false;
            }
            std::wstring rollback_error;
            if (!restore_backup_at_path(
                    target,
                    backup,
                    *persistent_acls,
                    *directory_guard,
                    &rollback_error)) {
                set_error(
                    error,
                    std::format(
                        L"替换失败且无法恢复测试目标：{}；备份：{}",
                        rollback_error,
                        backup.wstring()));
                return false;
            }
            set_error(error, L"替换失败，已恢复测试目标。");
            return false;
        }

        UniqueFile installed_guard =
            open_regular_file(target, FILE_SHARE_READ, error);
        if (!installed_guard ||
            (original_security &&
             !ensure_security_descriptor(
                 target,
                 *original_security,
                 &installed_guard,
                 error))) {
            installed_guard.reset();
            std::wstring rollback_error;
            if (!restore_backup_at_path(
                    target,
                    backup,
                    *persistent_acls,
                    *directory_guard,
                    &rollback_error)) {
                set_error(
                    error,
                    std::format(
                        L"安全描述符校验失败且无法恢复测试目标：{}；备份：{}",
                        rollback_error,
                        backup.wstring()));
            }
            if (error && error->empty()) {
                *error = L"测试替换后的安全描述符校验失败。";
            }
            return false;
        }
        clear_error(error);
        return true;
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return false;
    }
}

}  // namespace portable_internal

std::filesystem::path portable_executable_path() {
    std::wstring path(32'768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);
    return std::filesystem::path(path);
}

std::wstring portable_startup_command(const std::filesystem::path& executable) {
    return quote_argument(executable.wstring());
}

bool sync_portable_startup(bool enabled, std::wstring* error) {
    clear_error(error);
    HKEY key = nullptr;
    const LSTATUS opened = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRunKey,
        0,
        nullptr,
        0,
        KEY_QUERY_VALUE | KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (opened != ERROR_SUCCESS) {
        set_error(error, windows_error_message(opened));
        return false;
    }

    LSTATUS result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command =
            portable_startup_command(portable_executable_path());
        result = RegSetValueExW(
            key,
            kRunValue,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kRunValue);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        set_error(error, windows_error_message(result));
    }
    return result == ERROR_SUCCESS;
}

bool version_is_newer(std::wstring_view current, std::wstring_view candidate) {
    const auto current_parts = parse_version_parts(current);
    const auto candidate_parts = parse_version_parts(candidate);
    return current_parts && candidate_parts && *candidate_parts > *current_parts;
}

std::optional<UpdateManifest> parse_update_manifest(std::wstring_view json) {
    try {
        const JsonObject object = JsonObject::Parse(json);
        UpdateManifest manifest;
        manifest.version = object.GetNamedString(L"version").c_str();
        manifest.url = object.GetNamedString(L"url").c_str();
        manifest.sha256 =
            normalized_hex(object.GetNamedString(L"sha256").c_str());
        const double size = object.GetNamedNumber(L"size");
        if (!valid_version(manifest.version) ||
            !parse_https_url(manifest.url) || manifest.sha256.size() != 64 ||
            is_zero_hash(manifest.sha256) || !std::isfinite(size) ||
            size != std::trunc(size) || size <= 0 ||
            size > static_cast<double>(kMaxPortableUpdateBytes)) {
            return std::nullopt;
        }
        manifest.size = static_cast<std::uint64_t>(size);
        return manifest;
    } catch (...) {
        return std::nullopt;
    }
}

std::wstring update_manifest_to_json(const UpdateManifest& manifest) {
    JsonObject object;
    object.SetNamedValue(
        L"version", JsonValue::CreateStringValue(manifest.version));
    object.SetNamedValue(L"url", JsonValue::CreateStringValue(manifest.url));
    object.SetNamedValue(
        L"sha256",
        JsonValue::CreateStringValue(normalized_hex(manifest.sha256)));
    object.SetNamedValue(
        L"size",
        JsonValue::CreateNumberValue(static_cast<double>(manifest.size)));
    return object.Stringify().c_str();
}

std::wstring sha256_file(
    const std::filesystem::path& path,
    std::wstring* error) {
    clear_error(error);
    try {
        UniqueFile file = open_regular_file(path, FILE_SHARE_READ, error);
        return file ? sha256_handle(file.get(), error) : std::wstring{};
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return {};
    }
}

bool verify_portable_executable(
    const std::filesystem::path& path,
    const UpdateManifest& manifest,
    std::wstring* error) {
    clear_error(error);
    try {
        if (!valid_version(manifest.version) ||
            normalized_hex(manifest.sha256).size() != 64 ||
            is_zero_hash(normalized_hex(manifest.sha256)) ||
            manifest.size == 0 || manifest.size > kMaxPortableUpdateBytes) {
            set_error(error, L"更新清单内容无效。");
            return false;
        }
        UniqueFile guard = open_regular_file(path, FILE_SHARE_READ, error);
        if (!guard) {
            return false;
        }
        const auto size = file_size(guard.get());
        if (!size || *size != manifest.size) {
            set_error(error, L"更新程序大小与发布清单不一致。");
            return false;
        }
        if (sha256_handle(guard.get(), error) !=
            normalized_hex(manifest.sha256)) {
            if (error && error->empty()) {
                *error = L"更新程序 SHA256 校验失败。";
            }
            return false;
        }
        if (!authenticode_integrity_is_valid(path, error)) {
            return false;
        }
        const auto signer = signer_sha256(path);
        if (!signer || *signer != normalized_hex(kReleaseSignerSha256)) {
            set_error(
                error,
                L"更新程序签名证书与内置发布证书不一致。");
            return false;
        }
        const auto version = executable_version(path);
        if (!version || *version != manifest.version) {
            set_error(error, L"更新程序版本与发布清单不一致。");
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return false;
    }
}

bool update_target_is_replaceable(std::wstring* error) {
    clear_error(error);
    try {
        return target_directory_is_writable(error);
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return false;
    }
}

UpdateStageResult stage_latest_update(std::wstring* message) {
    return stage_latest_update(
        message, {}, UpdateRequestSource::automatic);
}

UpdateStageResult stage_latest_update(
    std::wstring* message,
    std::stop_token stop_token) {
    return stage_latest_update(
        message, stop_token, UpdateRequestSource::automatic);
}

UpdateStageResult stage_latest_update(
    std::wstring* message,
    std::stop_token stop_token,
    UpdateRequestSource source) {
    clear_error(message);
    auto update_lock =
        lock_update_operation(stop_token, INFINITE, message);
    if (!update_lock) {
        return UpdateStageResult::failed;
    }
    try {
        if (cancellation_requested(stop_token, message)) {
            return UpdateStageResult::failed;
        }
        std::wstring error;
        const auto directory = safe_updates_directory(true, &error);
        if (!directory) {
            set_error(message, error);
            return UpdateStageResult::failed;
        }
        auto update_directory_guard =
            lock_safe_directory_tree(*directory, &error);
        if (!update_directory_guard) {
            set_error(message, error);
            return UpdateStageResult::failed;
        }
        auto pending_update =
            validated_pending_update_unlocked(*directory);
        if (pending_update) {
            if (source == UpdateRequestSource::manual &&
                pending_update->source == UpdateRequestSource::automatic) {
                pending_update->source = UpdateRequestSource::manual;
                if (!write_text_file(
                        pending_manifest_path(*directory),
                        pending_update_to_json(*pending_update),
                        *update_directory_guard,
                        &error)) {
                    set_error(
                        message,
                        error.empty()
                            ? L"已下载更新有效，但无法记录本次手动更新意图。"
                            : error);
                    return UpdateStageResult::failed;
                }
            }
            set_error(
                message,
                L"新版本已下载，将在退出或下次启动时更新。");
            return UpdateStageResult::staged;
        }

        const auto latest_path =
            unique_path(*directory, L"latest", L".download");
        if (!latest_path) {
            set_error(message, L"无法创建更新清单临时文件。");
            return UpdateStageResult::failed;
        }
        ScopedPathCleanup latest_cleanup(*latest_path);
        if (!download_file(
                kLatestJsonUrl,
                *latest_path,
                kMaxManifestBytes,
                std::nullopt,
                *update_directory_guard,
                stop_token,
                &error)) {
            set_error(message, error);
            return UpdateStageResult::failed;
        }
        const auto latest_text = read_text_file(*latest_path, &error);
        if (cancellation_requested(stop_token, message)) {
            return UpdateStageResult::failed;
        }
        const auto manifest =
            latest_text ? parse_update_manifest(*latest_text) : std::nullopt;
        if (!manifest) {
            set_error(message,
                      error.empty() ? L"更新清单格式无效。" : error);
            return UpdateStageResult::failed;
        }
        if (!version_is_newer(
                from_utf8(AIRSHOT_VERSION), manifest->version)) {
            set_error(
                message,
                std::format(
                    L"当前已是最新版本 (v{})。",
                    from_utf8(AIRSHOT_VERSION)));
            return UpdateStageResult::up_to_date;
        }
        if (!target_directory_is_writable(&error)) {
            set_error(message, error);
            return UpdateStageResult::failed;
        }
        if (cancellation_requested(stop_token, message)) {
            return UpdateStageResult::failed;
        }

        const auto download_path =
            unique_path(*directory, L"payload", L".download");
        if (!download_path) {
            set_error(message, L"无法创建更新程序临时文件。");
            return UpdateStageResult::failed;
        }
        ScopedPathCleanup download_cleanup(*download_path);
        if (!download_file(manifest->url,
                           *download_path,
                           kMaxPortableUpdateBytes,
                           manifest->size,
                           *update_directory_guard,
                           stop_token,
                           &error)) {
            set_error(message, error);
            return UpdateStageResult::failed;
        }
        if (cancellation_requested(stop_token, message)) {
            return UpdateStageResult::failed;
        }
        if (!verify_portable_executable(
                *download_path, *manifest, &error)) {
            set_error(message, error);
            return UpdateStageResult::failed;
        }
        if (cancellation_requested(stop_token, message)) {
            return UpdateStageResult::failed;
        }

        const auto staged_path =
            update_executable_path(*directory, manifest->version);
        if (!atomic_move_file(
                *download_path,
                staged_path,
                *update_directory_guard,
                &error)) {
            set_error(message, error);
            return UpdateStageResult::failed;
        }
        download_cleanup.release();
        if (!write_text_file(
                pending_manifest_path(*directory),
                pending_update_to_json(PendingUpdate{*manifest, source}),
                *update_directory_guard,
                &error)) {
            remove_regular_file(staged_path);
            set_error(message, error);
            return UpdateStageResult::failed;
        }
        set_error(
            message,
            std::format(
                L"v{} 已下载，将在退出或下次启动时更新。",
                manifest->version));
        return UpdateStageResult::staged;
    } catch (const std::exception& exception) {
        set_error(message, from_utf8(exception.what()));
        return UpdateStageResult::failed;
    }
}

PendingUpdateManualResult mark_pending_update_manual(
    std::wstring* error) {
    clear_error(error);
    std::wstring lock_error;
    auto update_lock =
        lock_update_operation({}, 0, &lock_error);
    if (!update_lock) {
        set_error(error, lock_error);
        return lock_error.find(L"另一个更新操作正在进行") !=
                       std::wstring::npos
                   ? PendingUpdateManualResult::busy
                   : PendingUpdateManualResult::failed;
    }
    try {
        const std::filesystem::path expected_directory =
            updates_directory_path();
        if (path_is_missing(expected_directory)) {
            clear_error(error);
            return PendingUpdateManualResult::missing;
        }
        const auto directory = safe_updates_directory(false, error);
        if (!directory) {
            return PendingUpdateManualResult::failed;
        }
        auto directory_guard =
            lock_safe_directory_tree(*directory, error);
        if (!directory_guard) {
            return PendingUpdateManualResult::failed;
        }
        const auto path = pending_manifest_path(*directory);
        if (path_is_missing(path)) {
            clear_error(error);
            return PendingUpdateManualResult::missing;
        }
        const auto text = read_text_file(path, error);
        if (!text) {
            return PendingUpdateManualResult::failed;
        }
        auto pending = parse_pending_update(*text);
        if (!pending ||
            !version_is_newer(
                from_utf8(AIRSHOT_VERSION),
                pending->manifest.version)) {
            set_error(error, L"待处理更新清单无效或已经过期。");
            return PendingUpdateManualResult::invalid;
        }
        if (pending->source == UpdateRequestSource::manual) {
            return PendingUpdateManualResult::ready;
        }
        pending->source = UpdateRequestSource::manual;
        return write_text_file(
                   path,
                   pending_update_to_json(*pending),
                   *directory_guard,
                   error)
                   ? PendingUpdateManualResult::ready
                   : PendingUpdateManualResult::failed;
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return PendingUpdateManualResult::failed;
    }
}

bool launch_pending_update(
    bool restart_after_update,
    std::wstring* error) {
    return launch_pending_update(
        restart_after_update, true, error);
}

bool launch_pending_update(
    bool restart_after_update,
    bool allow_automatic_pending,
    std::wstring* error) {
    clear_error(error);
    auto update_lock =
        lock_update_operation({}, 15'000, error);
    if (!update_lock) {
        return false;
    }
    try {
        const std::filesystem::path expected_directory =
            updates_directory_path();
        if (path_is_missing(expected_directory)) {
            return false;
        }
        const auto directory = safe_updates_directory(false, error);
        if (!directory) {
            return false;
        }
        auto update_directory_guard =
            lock_safe_directory_tree(*directory, error);
        if (!update_directory_guard) {
            return false;
        }
        const std::filesystem::path pending =
            pending_manifest_path(*directory);
        if (path_is_missing(pending)) {
            clear_error(error);
            return false;
        }
        const auto text =
            read_text_file(pending, error);
        const auto pending_update =
            text ? parse_pending_update(*text) : std::nullopt;
        if (!pending_update ||
            !version_is_newer(
                from_utf8(AIRSHOT_VERSION),
                pending_update->manifest.version)) {
            if (text) {
                clear_error(error);
            }
            return false;
        }
        if (!portable_internal::pending_update_source_allowed(
                pending_update->source,
                allow_automatic_pending)) {
            clear_error(error);
            return false;
        }
        const UpdateManifest& manifest = pending_update->manifest;
        const auto staged =
            update_executable_path(*directory, manifest.version);
        if (!verify_portable_executable(staged, manifest, error)) {
            return false;
        }

        const auto target = portable_executable_path();
        if (target.empty() || !target_directory_is_writable(target, error)) {
            return false;
        }
        auto target_directory_guard =
            lock_safe_directory_tree(target.parent_path(), error);
        if (!target_directory_guard) {
            return false;
        }
        UniqueFile launch_guard =
            open_regular_file(staged, FILE_SHARE_READ, error);
        if (!launch_guard) {
            return false;
        }
        std::wstring ready_event_name;
        UniqueFile ready_event = create_update_ready_event(
            GetCurrentProcessId(), &ready_event_name, error);
        if (!ready_event) {
            return false;
        }
        const std::wstring command =
            std::format(L"{} --apply-update {} {} {} {}",
                        quote_argument(staged.wstring()),
                        quote_argument(target.wstring()),
                        GetCurrentProcessId(),
                        restart_after_update ? L"restart" : L"no-restart",
                        quote_argument(ready_event_name));
        UniqueFile helper_process;
        if (!create_process(
                staged, command, error, &helper_process)) {
            return false;
        }
        const std::array<HANDLE, 2> wait_handles{
            ready_event.get(), helper_process.get()};
        const auto handshake_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(15);
        const auto wait_for_handshake_phase =
            [&wait_handles, &handshake_deadline]() -> DWORD {
                const auto now =
                    std::chrono::steady_clock::now();
                if (now >= handshake_deadline) {
                    return WAIT_TIMEOUT;
                }
                const auto remaining =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        handshake_deadline - now);
                return WaitForMultipleObjects(
                    static_cast<DWORD>(wait_handles.size()),
                    wait_handles.data(),
                    FALSE,
                    static_cast<DWORD>(
                        std::max<long long>(
                            1, remaining.count())));
            };
        const auto report_handshake_failure =
            [&helper_process, error](DWORD wait_result) {
                if (wait_result == WAIT_OBJECT_0 + 1) {
                    DWORD exit_code = 0;
                    GetExitCodeProcess(
                        helper_process.get(), &exit_code);
                    set_error(
                        error,
                        std::format(
                            L"更新 helper 在安全启动握手前退出（代码 {}）。",
                            exit_code));
                    return false;
                }
                const DWORD wait_error =
                    wait_result == WAIT_FAILED
                        ? GetLastError()
                        : ERROR_SUCCESS;
                const DWORD already_exited =
                    WaitForSingleObject(
                        helper_process.get(), 0);
                DWORD terminate_error = ERROR_SUCCESS;
                if (already_exited != WAIT_OBJECT_0 &&
                    !TerminateProcess(
                        helper_process.get(),
                        static_cast<UINT>(
                            ExitCode::operation_failed))) {
                    terminate_error = GetLastError();
                }
                const DWORD terminated =
                    WaitForSingleObject(
                        helper_process.get(), 5'000);
                if (terminated != WAIT_OBJECT_0) {
                    set_error(
                        error,
                        std::format(
                            L"更新 helper 握手失败，且无法确认 helper 已停止（终止错误 {}，等待结果 {}）。",
                            terminate_error,
                            terminated));
                    return false;
                }
                if (wait_result == WAIT_TIMEOUT) {
                    set_error(
                        error,
                        L"更新 helper 未能在时限内完成安全启动握手，已停止 helper。");
                } else {
                    set_error(
                        error,
                        std::format(
                            L"更新 helper 安全启动握手失败：{} ({})，已停止 helper。",
                            windows_error_message(wait_error),
                            wait_error));
                }
                return false;
            };

        const DWORD validated =
            wait_for_handshake_phase();
        if (validated != WAIT_OBJECT_0) {
            return report_handshake_failure(validated);
        }
        if (!ResetEvent(ready_event.get())) {
            return report_handshake_failure(WAIT_FAILED);
        }
        update_lock.reset();

        const DWORD lease_acquired =
            wait_for_handshake_phase();
        if (lease_acquired != WAIT_OBJECT_0) {
            return report_handshake_failure(lease_acquired);
        }
        return true;
    } catch (const std::exception& exception) {
        set_error(error, from_utf8(exception.what()));
        return false;
    }
}

int run_update_helper(std::span<const std::wstring> arguments) {
    try {
        if (arguments.size() == 1 &&
            arguments[0] == L"--check-update-target") {
            return target_directory_is_writable(nullptr)
                       ? 0
                       : static_cast<int>(ExitCode::operation_failed);
        }
        if (arguments.size() == 3 &&
            arguments[0] == L"--verify-update") {
            const auto text = read_text_file(arguments[2]);
            const auto pending =
                text ? parse_pending_update(*text) : std::nullopt;
            return pending &&
                           verify_portable_executable(
                               arguments[1], pending->manifest)
                       ? 0
                       : static_cast<int>(ExitCode::operation_failed);
        }
        const bool legacy_apply_arguments = arguments.size() == 4;
        const bool handshake_apply_arguments = arguments.size() == 5;
        if ((!legacy_apply_arguments && !handshake_apply_arguments) ||
            arguments[0] != L"--apply-update" ||
            (arguments[3] != L"restart" &&
             arguments[3] != L"no-restart")) {
            return static_cast<int>(ExitCode::invalid_arguments);
        }

        const auto process_id = parse_process_id(arguments[2]);
        std::filesystem::path target(arguments[1]);
        if (!process_id || !target.is_absolute() ||
            (handshake_apply_arguments &&
             !valid_update_ready_event_name(
                  arguments[4], *process_id))) {
            return static_cast<int>(ExitCode::invalid_arguments);
        }
        target = std::filesystem::absolute(target).lexically_normal();

        std::wstring error;
        const auto parent_process_id = current_parent_process_id();
        if (!parent_process_id || *parent_process_id != *process_id) {
            return static_cast<int>(ExitCode::operation_failed);
        }

        const auto directory = safe_updates_directory(false, &error);
        UpdateManifest manifest;
        if (!directory) {
            return static_cast<int>(ExitCode::operation_failed);
        }
        auto update_directory_guard =
            lock_safe_directory_tree(*directory, &error);
        if (!update_directory_guard) {
            return static_cast<int>(ExitCode::operation_failed);
        }
        if (!helper_matches_pending_update(
                *directory, &manifest, &error) &&
            (!legacy_apply_arguments ||
             !helper_is_trusted_legacy_update(
                 *directory, &manifest, &error))) {
            return static_cast<int>(ExitCode::operation_failed);
        }

        UniqueFile process(OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            *process_id));
        if (!process) {
            const DWORD open_error = GetLastError();
            if (!legacy_apply_arguments ||
                open_error != ERROR_INVALID_PARAMETER) {
                return static_cast<int>(ExitCode::operation_failed);
            }
        } else {
            const auto parent_image =
                process_image_path(process.get());
            if (!parent_image ||
                !same_file(target, *parent_image, &error) ||
                !process_started_no_later_than(
                    process.get(), GetCurrentProcess())) {
                return static_cast<int>(ExitCode::operation_failed);
            }
        }

        UniqueFile ready_event;
        if (handshake_apply_arguments) {
            if (!process) {
                return static_cast<int>(ExitCode::operation_failed);
            }
            ready_event.reset(OpenEventW(
                EVENT_MODIFY_STATE, FALSE, arguments[4].c_str()));
            if (!ready_event) {
                return static_cast<int>(ExitCode::operation_failed);
            }
        }

        FileIdentity original_identity;
        UniqueFile original_target;
        std::optional<UpdateOperationLock> helper_update_lock;
        if (handshake_apply_arguments) {
            if (!verify_existing_install(
                    target,
                    manifest.version,
                    &original_identity,
                    &original_target,
                    &error) ||
                !SetEvent(ready_event.get())) {
                return static_cast<int>(ExitCode::operation_failed);
            }
            helper_update_lock =
                lock_update_operation({}, 30'000, &error);
            if (!helper_update_lock ||
                !SetEvent(ready_event.get())) {
                return static_cast<int>(ExitCode::operation_failed);
            }
        } else {
            helper_update_lock =
                lock_update_operation({}, 30'000, &error);
            if (!helper_update_lock ||
                !verify_existing_install(
                    target,
                    manifest.version,
                    &original_identity,
                    &original_target,
                    &error)) {
                return static_cast<int>(ExitCode::operation_failed);
            }
        }
        ready_event.reset();
        if (process) {
            const DWORD wait_result =
                WaitForSingleObject(process.get(), 30'000);
            if (wait_result != WAIT_OBJECT_0) {
                return static_cast<int>(ExitCode::operation_failed);
            }
            process.reset();
        }

        StableTargetGuard stable_target;
        if (!target_directory_is_writable(target, &error) ||
            !atomic_replace_target(
                target,
                manifest,
                original_identity,
                &original_target,
                &stable_target,
                &error)) {
            MessageBoxW(
                nullptr,
                error.empty()
                    ? L"无法安全替换 AirScreenshot.exe。请将程序移到可写目录后重试。"
                    : error.c_str(),
                kAppName,
                MB_OK | MB_ICONERROR);
            return static_cast<int>(ExitCode::operation_failed);
        }

        remove_regular_file(pending_manifest_path(*directory));
        if (arguments[3] == L"restart") {
            std::wstring command =
                quote_argument(target.wstring());
            if (!create_process(target, std::move(command), nullptr)) {
                return static_cast<int>(ExitCode::operation_failed);
            }
        }
        return 0;
    } catch (...) {
        return static_cast<int>(ExitCode::operation_failed);
    }
}

void cleanup_stale_updates() {
    auto update_lock =
        lock_update_operation({}, 0, nullptr);
    if (!update_lock) {
        return;
    }
    try {
        const auto directory = safe_updates_directory(false);
        if (!directory) {
            return;
        }
        auto directory_guard =
            lock_safe_directory_tree(*directory, nullptr);
        if (!directory_guard ||
            pending_update_available_unlocked(*directory)) {
            return;
        }
        remove_regular_file(pending_manifest_path(*directory));

        std::error_code filesystem_error;
        for (const auto& entry :
             std::filesystem::directory_iterator(
                 *directory,
                 std::filesystem::directory_options::skip_permission_denied,
                 filesystem_error)) {
            if (filesystem_error) {
                break;
            }
            const auto status = entry.symlink_status(filesystem_error);
            if (filesystem_error ||
                status.type() != std::filesystem::file_type::regular) {
                filesystem_error.clear();
                continue;
            }
            const std::wstring filename =
                entry.path().filename().wstring();
            if (staged_filename(filename) ||
                temporary_update_filename(filename)) {
                remove_regular_file(entry.path());
            }
        }
    } catch (...) {
    }
}

}  // namespace airshot
