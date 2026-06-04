#include "airshot/portable.h"

#include "airshot/config.h"

#include <bcrypt.h>
#include <softpub.h>
#include <urlmon.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <array>
#include <fstream>
#include <mutex>

#include <winrt/Windows.Data.Json.h>

namespace airshot {
namespace {

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"AirScreenshot";

std::filesystem::path updates_directory() {
    return config_directory() / L"updates";
}

std::filesystem::path pending_manifest_path() {
    return updates_directory() / L"pending.json";
}

std::filesystem::path update_executable_path(std::wstring_view version) {
    return updates_directory() / std::format(L"AirScreenshot-{}.exe", version);
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

bool valid_version(std::wstring_view value) {
    int separators = 0;
    bool has_digit = false;
    for (const wchar_t character : value) {
        if (character == L'.') {
            if (!has_digit) {
                return false;
            }
            ++separators;
            has_digit = false;
        } else if (character >= L'0' && character <= L'9') {
            has_digit = true;
        } else {
            return false;
        }
    }
    return separators == 2 && has_digit;
}

std::vector<unsigned long long> version_parts(std::wstring_view value) {
    std::vector<unsigned long long> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(L'.', start);
        const std::wstring part(value.substr(start, end == std::wstring_view::npos ? value.size() - start : end - start));
        try {
            result.push_back(std::stoull(part));
        } catch (...) {
            return {};
        }
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

bool write_text_file(const std::filesystem::path& path, std::wstring_view text, std::wstring* error) {
    try {
        std::filesystem::create_directories(path.parent_path());
        const auto temporary = path.parent_path() / (path.filename().wstring() + L".tmp");
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        const std::string bytes = to_utf8(text);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream) {
            throw std::runtime_error("write failed");
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(to_utf8(windows_error_message(GetLastError())));
        }
        return true;
    } catch (const std::exception& exception) {
        if (error) {
            *error = from_utf8(exception.what());
        }
        return false;
    }
}

std::optional<std::wstring> read_text_file(const std::filesystem::path& path, std::wstring* error = nullptr) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
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

bool target_directory_is_writable(std::wstring* error) {
    const DWORD attributes = GetFileAttributesW(portable_executable_path().c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        if (error) {
            *error = L"当前 AirScreenshot.exe 是只读文件，请取消只读属性或移到普通文件夹后再运行。";
        }
        return false;
    }
    const auto directory = portable_executable_path().parent_path();
    const auto probe = directory / std::format(L".air-screenshot-write-test-{}.tmp", GetCurrentProcessId());
    HANDLE file = CreateFileW(probe.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = L"当前目录不可写，请将 AirScreenshot.exe 移到普通文件夹后再运行。";
        }
        return false;
    }
    CloseHandle(file);
    return true;
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
    if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signer_bytes.data(), &signer_size)) {
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
        if (CertGetCertificateContextProperty(certificate, CERT_SHA256_HASH_PROP_ID, nullptr, &hash_size)) {
            std::vector<std::uint8_t> hash(hash_size);
            if (CertGetCertificateContextProperty(
                    certificate, CERT_SHA256_HASH_PROP_ID, hash.data(), &hash_size)) {
                std::wstring value;
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

bool authenticode_integrity_is_valid(const std::filesystem::path& path, std::wstring* error) {
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

    if (status == ERROR_SUCCESS || status == CERT_E_UNTRUSTEDROOT || status == CERT_E_CHAINING ||
        status == TRUST_E_SUBJECT_NOT_TRUSTED) {
        return true;
    }
    if (error) {
        *error = std::format(L"更新程序签名无效：0x{:08X}", static_cast<unsigned int>(status));
    }
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
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
        !info || info_size < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE) {
        return std::nullopt;
    }
    return std::format(L"{}.{}.{}",
                       HIWORD(info->dwProductVersionMS),
                       LOWORD(info->dwProductVersionMS),
                       HIWORD(info->dwProductVersionLS));
}

bool create_process(const std::filesystem::path& executable, std::wstring command, std::wstring* error) {
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
        if (error) {
            *error = windows_error_message(GetLastError());
        }
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

}  // namespace

std::filesystem::path portable_executable_path() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path);
}

std::wstring portable_startup_command(const std::filesystem::path& executable) {
    return quote_argument(executable.wstring());
}

bool sync_portable_startup(bool enabled, std::wstring* error) {
    HKEY key = nullptr;
    const LSTATUS opened = RegCreateKeyExW(
        HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS) {
        if (error) {
            *error = windows_error_message(opened);
        }
        return false;
    }

    LSTATUS result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = portable_startup_command(portable_executable_path());
        result = RegSetValueExW(key,
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
    if (result != ERROR_SUCCESS && error) {
        *error = windows_error_message(result);
    }
    return result == ERROR_SUCCESS;
}

bool version_is_newer(std::wstring_view current, std::wstring_view candidate) {
    if (!valid_version(current) || !valid_version(candidate)) {
        return false;
    }
    const auto current_parts = version_parts(current);
    const auto candidate_parts = version_parts(candidate);
    return candidate_parts.size() == 3 && current_parts.size() == 3 && candidate_parts > current_parts;
}

std::optional<UpdateManifest> parse_update_manifest(std::wstring_view json) {
    try {
        const JsonObject object = JsonObject::Parse(json);
        UpdateManifest manifest;
        manifest.version = object.GetNamedString(L"version").c_str();
        manifest.url = object.GetNamedString(L"url").c_str();
        manifest.sha256 = normalized_hex(object.GetNamedString(L"sha256").c_str());
        const double size = object.GetNamedNumber(L"size");
        if (!valid_version(manifest.version) || !manifest.url.starts_with(L"https://") ||
            manifest.sha256.size() != 64 || size <= 0 || size > 50.0 * 1024.0 * 1024.0) {
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
    object.SetNamedValue(L"version", JsonValue::CreateStringValue(manifest.version));
    object.SetNamedValue(L"url", JsonValue::CreateStringValue(manifest.url));
    object.SetNamedValue(L"sha256", JsonValue::CreateStringValue(normalized_hex(manifest.sha256)));
    object.SetNamedValue(L"size", JsonValue::CreateNumberValue(static_cast<double>(manifest.size)));
    return object.Stringify().c_str();
}

std::wstring sha256_file(const std::filesystem::path& path, std::wstring* error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::wstring result;
    HANDLE file = INVALID_HANDLE_VALUE;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        if (error) {
            *error = L"无法初始化 SHA256 校验。";
        }
        if (algorithm) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        return {};
    }

    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = windows_error_message(GetLastError());
        }
    } else {
        std::vector<std::uint8_t> buffer(64 * 1024);
        DWORD read = 0;
        bool ok = true;
        BOOL read_result = TRUE;
        while ((read_result = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) &&
               read > 0) {
            if (BCryptHashData(hash, buffer.data(), read, 0) != 0) {
                ok = false;
                break;
            }
        }
        if (!read_result) {
            ok = false;
            if (error) {
                *error = windows_error_message(GetLastError());
            }
        }
        if (ok) {
            std::array<std::uint8_t, 32> digest{};
            if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
                for (const std::uint8_t byte : digest) {
                    result += std::format(L"{:02X}", byte);
                }
            }
        }
        CloseHandle(file);
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

bool verify_portable_executable(const std::filesystem::path& path, const UpdateManifest& manifest, std::wstring* error) {
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size != manifest.size) {
        if (error) {
            *error = L"更新程序大小与发布清单不一致。";
        }
        return false;
    }
    if (sha256_file(path, error) != normalized_hex(manifest.sha256)) {
        if (error && error->empty()) {
            *error = L"更新程序 SHA256 校验失败。";
        }
        return false;
    }
    if (!authenticode_integrity_is_valid(path, error)) {
        return false;
    }
    const auto signer = signer_sha256(path);
    if (!signer || *signer != kReleaseSignerSha256) {
        if (error) {
            *error = L"更新程序签名证书与内置发布证书不一致。";
        }
        return false;
    }
    const auto version = executable_version(path);
    if (!version || *version != manifest.version) {
        if (error) {
            *error = L"更新程序版本与发布清单不一致。";
        }
        return false;
    }
    return true;
}

UpdateStageResult stage_latest_update(std::wstring* message) {
    static std::mutex update_mutex;
    const std::scoped_lock lock(update_mutex);
    if (pending_update_available()) {
        if (message) {
            *message = L"新版本已下载，将在退出或下次启动时更新。";
        }
        return UpdateStageResult::staged;
    }
    std::wstring error;
    const auto directory = updates_directory();
    const auto latest_path = directory / L"latest.json.download";
    if (!download_file(kLatestJsonUrl, latest_path, &error)) {
        if (message) {
            *message = error;
        }
        return UpdateStageResult::failed;
    }
    const auto latest_text = read_text_file(latest_path, &error);
    std::error_code ignored;
    std::filesystem::remove(latest_path, ignored);
    const auto manifest = latest_text ? parse_update_manifest(*latest_text) : std::nullopt;
    if (!manifest) {
        if (message) {
            *message = L"更新清单格式无效。";
        }
        return UpdateStageResult::failed;
    }
    if (!version_is_newer(from_utf8(AIRSHOT_VERSION), manifest->version)) {
        if (message) {
            *message = std::format(L"当前已是最新版本 (v{})。", from_utf8(AIRSHOT_VERSION));
        }
        return UpdateStageResult::up_to_date;
    }
    if (!target_directory_is_writable(&error)) {
        if (message) {
            *message = error;
        }
        return UpdateStageResult::failed;
    }

    const auto download_path = directory / L"AirScreenshot.download";
    if (!download_file(manifest->url, download_path, &error) ||
        !verify_portable_executable(download_path, *manifest, &error)) {
        std::filesystem::remove(download_path, ignored);
        if (message) {
            *message = error;
        }
        return UpdateStageResult::failed;
    }
    const auto staged_path = update_executable_path(manifest->version);
    std::filesystem::remove(staged_path, ignored);
    std::filesystem::rename(download_path, staged_path, ignored);
    if (ignored || !write_text_file(pending_manifest_path(), update_manifest_to_json(*manifest), &error)) {
        if (message) {
            *message = ignored ? L"无法保存更新程序。" : error;
        }
        return UpdateStageResult::failed;
    }
    if (message) {
        *message = std::format(L"v{} 已下载，将在退出或下次启动时更新。", manifest->version);
    }
    return UpdateStageResult::staged;
}

bool pending_update_available() {
    const auto text = read_text_file(pending_manifest_path());
    const auto manifest = text ? parse_update_manifest(*text) : std::nullopt;
    if (!manifest || !version_is_newer(from_utf8(AIRSHOT_VERSION), manifest->version)) {
        return false;
    }
    return verify_portable_executable(update_executable_path(manifest->version), *manifest);
}

bool launch_pending_update(bool restart_after_update, std::wstring* error) {
    const auto text = read_text_file(pending_manifest_path(), error);
    const auto manifest = text ? parse_update_manifest(*text) : std::nullopt;
    if (!manifest || !version_is_newer(from_utf8(AIRSHOT_VERSION), manifest->version)) {
        return false;
    }
    const auto staged = update_executable_path(manifest->version);
    if (!verify_portable_executable(staged, *manifest, error)) {
        return false;
    }

    const auto target = portable_executable_path();
    if (!target_directory_is_writable(error)) {
        return false;
    }
    const std::wstring command = std::format(L"{} --apply-update {} {} {}",
                                             quote_argument(staged.wstring()),
                                             quote_argument(target.wstring()),
                                             GetCurrentProcessId(),
                                             restart_after_update ? L"restart" : L"no-restart");
    if (!create_process(staged, command, error)) {
        return false;
    }
    std::error_code ignored;
    std::filesystem::remove(pending_manifest_path(), ignored);
    return true;
}

int run_update_helper(std::span<const std::wstring> arguments) {
    if (arguments.size() == 1 && arguments[0] == L"--check-update-target") {
        return target_directory_is_writable(nullptr) ? 0 : static_cast<int>(ExitCode::operation_failed);
    }
    if (arguments.size() == 3 && arguments[0] == L"--verify-update") {
        const auto text = read_text_file(arguments[2]);
        const auto manifest = text ? parse_update_manifest(*text) : std::nullopt;
        return manifest && verify_portable_executable(arguments[1], *manifest)
                   ? 0
                   : static_cast<int>(ExitCode::operation_failed);
    }
    if (arguments.size() != 4 || arguments[0] != L"--apply-update") {
        return static_cast<int>(ExitCode::invalid_arguments);
    }
    const std::filesystem::path target(arguments[1]);
    DWORD process_id = 0;
    try {
        process_id = static_cast<DWORD>(std::stoul(arguments[2]));
    } catch (...) {
        return static_cast<int>(ExitCode::invalid_arguments);
    }
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id)) {
        WaitForSingleObject(process, 30000);
        CloseHandle(process);
    }

    bool copied = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (CopyFileW(portable_executable_path().c_str(), target.c_str(), FALSE)) {
            copied = true;
            break;
        }
        Sleep(50);
    }
    if (!copied) {
        MessageBoxW(nullptr,
                    L"无法替换 AirScreenshot.exe。请将程序移到可写目录后重试。",
                    kAppName,
                    MB_OK | MB_ICONERROR);
        return static_cast<int>(ExitCode::operation_failed);
    }
    if (arguments[3] == L"restart") {
        std::wstring command = quote_argument(target.wstring());
        create_process(target, std::move(command), nullptr);
    }
    return 0;
}

void cleanup_stale_updates() {
    if (pending_update_available()) {
        return;
    }
    std::error_code ignored;
    std::filesystem::remove(pending_manifest_path(), ignored);
    for (const auto& entry : std::filesystem::directory_iterator(updates_directory(), ignored)) {
        if (entry.is_regular_file() && entry.path().extension() == L".exe") {
            std::filesystem::remove(entry.path(), ignored);
        }
    }
}

}  // namespace airshot
