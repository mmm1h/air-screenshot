#include "airshot/ocr.h"
#include "airshot/config.h"
#include "ocr_test_support.h"

#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::wstring_view name) {
    if (!condition) {
        std::wcerr << L"FAIL: " << name << L"\n";
        ++failures;
    }
}

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE value) noexcept
        : value_(value) {}
    ~ScopedHandle() {
        reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    void reset(HANDLE value = nullptr) noexcept {
        if (value_ &&
            value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_{};
};

class ScopedTestDirectory {
public:
    ScopedTestDirectory() {
        std::wstring temporary_root(
            32768,
            L'\0');
        const DWORD root_size = GetTempPathW(
            static_cast<DWORD>(
                temporary_root.size()),
            temporary_root.data());
        if (root_size == 0 ||
            root_size >= temporary_root.size()) {
            return;
        }
        temporary_root.resize(root_size);

        GUID guid{};
        wchar_t guid_text[40]{};
        if (FAILED(CoCreateGuid(&guid)) ||
            StringFromGUID2(
                guid,
                guid_text,
                static_cast<int>(
                    std::size(guid_text))) <= 0) {
            return;
        }
        path_ =
            std::filesystem::path(temporary_root) /
            (L"AirScreenshot.OcrTest." +
             std::wstring(guid_text));
        if (!CreateDirectoryW(
                path_.c_str(),
                nullptr)) {
            path_.clear();
        }
    }

    ~ScopedTestDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(
                path_,
                ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path&
    path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::wstring> required_paths() {
    std::vector<std::wstring> paths{
        L"rapidocr_runner.exe",
        L"onnxruntime.dll",
        L"msvcp140.dll",
        L"msvcp140_1.dll",
        L"vcruntime140.dll",
        L"vcruntime140_1.dll",
    };
    for (const std::wstring_view profile :
         {L"rapidocr-v5-fast", L"rapidocr-v5-accurate", L"rapidocr-v4-compat"}) {
        for (const std::wstring_view file :
             {L"det.onnx", L"rec.onnx", L"cls.onnx", L"dict.txt"}) {
            paths.push_back(
                L"models/" + std::wstring(profile) + L"/" + std::wstring(file));
        }
    }
    return paths;
}

std::wstring manifest_json(std::span<const std::wstring> extra_paths = {}) {
    const std::vector<std::wstring> required = required_paths();
    std::wstring result =
        LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[)";
    std::size_t index = 0;
    auto append_file = [&](std::wstring_view path) {
        if (index != 0) {
            result += L",";
        }
        result += LR"({"path":")";
        result += path;
        result += LR"(","url":"https://example.com/)";
        result += path;
        result +=
            LR"(","sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","size":)";
        result += std::to_wstring(++index);
        result += L"}";
    };
    for (const auto& path : required) {
        append_file(path);
    }
    for (const auto& path : extra_paths) {
        append_file(path);
    }
    result += L"]}";
    return result;
}

std::vector<std::byte> as_bytes(std::string_view value) {
    std::vector<std::byte> result(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        result[index] = static_cast<std::byte>(
            static_cast<unsigned char>(value[index]));
    }
    return result;
}

class AlgorithmHandle {
public:
    ~AlgorithmHandle() {
        if (value) {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }
    BCRYPT_ALG_HANDLE value{};
};

class HashHandle {
public:
    ~HashHandle() {
        if (value) {
            BCryptDestroyHash(value);
        }
    }
    BCRYPT_HASH_HANDLE value{};
};

class KeyHandle {
public:
    ~KeyHandle() {
        if (value) {
            BCryptDestroyKey(value);
        }
    }
    BCRYPT_KEY_HANDLE value{};
};

struct SignatureFixture {
    std::vector<std::byte> manifest;
    std::vector<std::uint8_t> signature;
    std::array<std::uint8_t, 64> public_key_xy{};
};

std::optional<SignatureFixture> make_signature_fixture() {
    SignatureFixture fixture;
    fixture.manifest =
        as_bytes(R"({"packageId":"rapidocr-onnx","files":[]})");

    AlgorithmHandle sha_algorithm;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &sha_algorithm.value,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0))) {
        return std::nullopt;
    }

    DWORD object_size = 0;
    DWORD copied = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(
            sha_algorithm.value,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size),
            sizeof(object_size),
            &copied,
            0))) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> hash_object(object_size);
    std::array<std::uint8_t, 32> digest{};
    HashHandle hash;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(
            sha_algorithm.value,
            &hash.value,
            hash_object.data(),
            static_cast<ULONG>(hash_object.size()),
            nullptr,
            0,
            0)) ||
        !BCRYPT_SUCCESS(BCryptHashData(
            hash.value,
            reinterpret_cast<PUCHAR>(fixture.manifest.data()),
            static_cast<ULONG>(fixture.manifest.size()),
            0)) ||
        !BCRYPT_SUCCESS(BCryptFinishHash(
            hash.value,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            0))) {
        return std::nullopt;
    }

    AlgorithmHandle ecdsa_algorithm;
    KeyHandle key;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &ecdsa_algorithm.value,
            BCRYPT_ECDSA_P256_ALGORITHM,
            nullptr,
            0)) ||
        !BCRYPT_SUCCESS(BCryptGenerateKeyPair(
            ecdsa_algorithm.value,
            &key.value,
            256,
            0)) ||
        !BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key.value, 0))) {
        return std::nullopt;
    }

    ULONG public_blob_size = 0;
    if (!BCRYPT_SUCCESS(BCryptExportKey(
            key.value,
            nullptr,
            BCRYPT_ECCPUBLIC_BLOB,
            nullptr,
            0,
            &public_blob_size,
            0))) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> public_blob(public_blob_size);
    if (!BCRYPT_SUCCESS(BCryptExportKey(
            key.value,
            nullptr,
            BCRYPT_ECCPUBLIC_BLOB,
            public_blob.data(),
            static_cast<ULONG>(public_blob.size()),
            &public_blob_size,
            0)) ||
        public_blob.size() !=
            sizeof(BCRYPT_ECCKEY_BLOB) + fixture.public_key_xy.size()) {
        return std::nullopt;
    }
    std::copy(
        public_blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB),
        public_blob.end(),
        fixture.public_key_xy.begin());

    ULONG signature_size = 0;
    if (!BCRYPT_SUCCESS(BCryptSignHash(
            key.value,
            nullptr,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            nullptr,
            0,
            &signature_size,
            0))) {
        return std::nullopt;
    }
    fixture.signature.resize(signature_size);
    if (!BCRYPT_SUCCESS(BCryptSignHash(
            key.value,
            nullptr,
            digest.data(),
            static_cast<ULONG>(digest.size()),
            fixture.signature.data(),
            static_cast<ULONG>(fixture.signature.size()),
            &signature_size,
            0)) ||
        signature_size != 64) {
        return std::nullopt;
    }
    fixture.signature.resize(signature_size);
    return fixture;
}

void test_manifest_parser() {
    const auto valid =
        airshot::parse_ocr_dependency_manifest(manifest_json());
    expect(
        valid && valid->files.size() == required_paths().size(),
        L"OCR manifest accepts the complete native runtime payload");

    std::wstring fractional = manifest_json();
    const std::size_t size_position = fractional.find(LR"("size":1)");
    fractional.replace(size_position, std::wstring_view(LR"("size":1)").size(), LR"("size":1.5)");
    expect(
        !airshot::parse_ocr_dependency_manifest(fractional),
        L"OCR manifest rejects fractional sizes");

    std::wstring fractional_sequence = manifest_json();
    const std::size_t sequence_position =
        fractional_sequence.find(LR"("sequence":1000000)");
    fractional_sequence.replace(
        sequence_position,
        std::wstring_view(LR"("sequence":1000000)").size(),
        LR"("sequence":1000000.5)");
    expect(
        !airshot::parse_ocr_dependency_manifest(fractional_sequence),
        L"OCR manifest rejects fractional sequence");

    std::wstring excessive_lifetime = manifest_json();
    const std::size_t expiry_position =
        excessive_lifetime.find(LR"("expiresAt":1810000000)");
    excessive_lifetime.replace(
        expiry_position,
        std::wstring_view(LR"("expiresAt":1810000000)").size(),
        LR"("expiresAt":1900000000)");
    expect(
        !airshot::parse_ocr_dependency_manifest(excessive_lifetime),
        L"OCR manifest rejects excessive validity lifetime");

    const std::array duplicate_path{std::wstring(L"RAPIDOCR_RUNNER.EXE")};
    expect(
        !airshot::parse_ocr_dependency_manifest(
            manifest_json(duplicate_path)),
        L"OCR manifest rejects case-insensitive duplicate paths");

    const std::array reserved_path{std::wstring(L"models/CON.txt")};
    expect(
        !airshot::parse_ocr_dependency_manifest(
            manifest_json(reserved_path)),
        L"OCR manifest rejects Windows reserved path components");

    const std::array trailing_path{std::wstring(L"models/name./file.bin")};
    expect(
        !airshot::parse_ocr_dependency_manifest(
            manifest_json(trailing_path)),
        L"OCR manifest rejects trailing-dot path components");

    const std::array non_ascii_path{std::wstring(L"models/\u00C5/file.bin")};
    expect(
        !airshot::parse_ocr_dependency_manifest(
            manifest_json(non_ascii_path)),
        L"OCR manifest rejects non-ASCII dependency paths");
}

void test_signature_parser() {
    const std::wstring valid_json =
        LR"({"keyId":"production-2026","signature":")" +
        std::wstring(128, L'A') + L"\"}";
    const auto valid = airshot::parse_ocr_manifest_signature(valid_json);
    expect(
        valid && valid->key_id == L"production-2026",
        L"OCR detached signature JSON parses");

    expect(
        !airshot::parse_ocr_manifest_signature(
            LR"({"keyId":"bad key","signature":")" +
            std::wstring(128, L'A') + L"\"}"),
        L"OCR detached signature rejects unsafe key id");
    expect(
        !airshot::parse_ocr_manifest_signature(
            LR"({"keyId":"production","signature":"AA"})"),
        L"OCR detached signature rejects incorrect length");
    expect(
        !airshot::parse_ocr_manifest_signature(
            LR"({"keyId":"production","signature":")" +
            std::wstring(128, L'Z') + L"\"}"),
        L"OCR detached signature rejects non-hex data");
    expect(
        !airshot::parse_ocr_manifest_signature(
            LR"({"keyId":"production","signature":")" +
            std::wstring(128, L'A') + LR"(","extra":true})"),
        L"OCR detached signature rejects unknown fields");
}

void test_signature_verifier() {
    auto fixture = make_signature_fixture();
    expect(fixture.has_value(), L"create P-256 test signature");
    if (!fixture) {
        return;
    }

    std::wstring error;
    expect(
        airshot::verify_ocr_manifest_signature(
            fixture->manifest,
            fixture->signature,
            fixture->public_key_xy,
            &error),
        L"OCR ECDSA verifier accepts valid P1363 signature");

    fixture->manifest[0] ^= std::byte{1};
    expect(
        !airshot::verify_ocr_manifest_signature(
            fixture->manifest,
            fixture->signature,
            fixture->public_key_xy,
            &error),
        L"OCR ECDSA verifier rejects tampered manifest");
    fixture->manifest[0] ^= std::byte{1};

    fixture->signature[0] ^= 1;
    expect(
        !airshot::verify_ocr_manifest_signature(
            fixture->manifest,
            fixture->signature,
            fixture->public_key_xy,
            &error),
        L"OCR ECDSA verifier rejects tampered signature");
}

void test_pre_requested_download_cancellation() {
    std::stop_source stop;
    stop.request_stop();
    std::wstring error;
    int progress_calls = 0;
    expect(
        !airshot::download_ocr_dependencies(
            L"https://127.0.0.1/this-must-not-be-requested.json",
            [&progress_calls](int) {
                ++progress_calls;
            },
            &error,
            stop.get_token()),
        L"OCR downloader honors pre-requested cancellation");
    expect(
        error.find(L"取消") != std::wstring::npos,
        L"OCR downloader reports cancellation");
    expect(
        progress_calls == 0,
        L"OCR pre-cancel performs no progress or network work");
}

void test_unconfigured_build_is_offline_and_unavailable() {
    ScopedTestDirectory directory;
    expect(!directory.path().empty(),
           L"OCR unavailable-state test creates an isolated data directory");
    if (directory.path().empty()) {
        return;
    }

    const DWORD old_size =
        GetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr, 0);
    std::wstring old_value(old_size, L'\0');
    if (old_size > 0) {
        const DWORD copied = GetEnvironmentVariableW(
            L"AIRSHOT_DATA_DIR", old_value.data(), old_size);
        old_value.resize(copied);
    }
    const std::filesystem::path isolated_data =
        directory.path() / L"data";
    SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", isolated_data.c_str());

    const airshot::OcrDependencyStatus status =
        airshot::ocr_dependency_status(L"rapidocr-onnx");
    if (!status.ready && !status.can_download) {
        expect(
            status.message.find(L"公钥") != std::wstring::npos &&
                status.state == airshot::OcrDependencyState::unavailable &&
                status.recommended_action ==
                    airshot::OcrRecoveryAction::configure_build &&
                status.security_blocked && !status.retryable,
            L"an unconfigured build reports a structured security-blocked OCR state");

        int progress_calls = 0;
        std::wstring error;
        const auto started = std::chrono::steady_clock::now();
        const bool downloaded = airshot::download_ocr_dependencies(
            L"https://127.0.0.1:9/this-must-not-be-requested.json",
            [&progress_calls](int) { ++progress_calls; },
            &error);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(
            !downloaded && progress_calls == 0 &&
                error.find(L"公钥") != std::wstring::npos &&
                error.find(L"拒绝网络安装") != std::wstring::npos &&
                elapsed < std::chrono::seconds(2),
            L"an unconfigured build refuses OCR installation before network I/O");
    }

    const airshot::OcrPreparationOptions offline_options{
        std::wstring(airshot::kOcrEngineRapidV5Fast),
        L"https://127.0.0.1:9/this-must-not-be-requested.json",
        false,
    };
    std::vector<airshot::OcrPreparationProgress> offline_progress;
    const auto offline_result = airshot::prepare_ocr_dependencies(
        offline_options,
        [&offline_progress](const airshot::OcrPreparationProgress& progress) {
            offline_progress.push_back(progress);
        });
    if (status.ready) {
        expect(
            offline_result.status.ready && offline_result.used_existing &&
                offline_result.status.state ==
                    airshot::OcrDependencyState::ready &&
                offline_result.status.usable_offline,
            L"first-use preparation reuses a verified packaged OCR component offline");
    } else if (status.can_download) {
        expect(
            !offline_result.status.ready &&
                offline_result.status.state ==
                    airshot::OcrDependencyState::offline &&
                offline_result.status.recommended_action ==
                    airshot::OcrRecoveryAction::go_online &&
                offline_result.status.requires_network &&
                offline_result.status.retryable,
            L"first-use preparation stays offline when network use is disabled");
    } else {
        expect(
            offline_result.status.state ==
                    airshot::OcrDependencyState::unavailable &&
                offline_result.status.recommended_action ==
                    airshot::OcrRecoveryAction::configure_build &&
                offline_result.status.security_blocked,
            L"first-use preparation preserves the build trust prerequisite offline");
    }
    expect(
        offline_progress.size() >= 2 &&
            offline_progress.front().state ==
                airshot::OcrDependencyState::checking &&
            offline_progress.back().state == offline_result.status.state,
        L"first-use preparation publishes stable checking and terminal states");

    std::stop_source preparation_stop;
    preparation_stop.request_stop();
    std::vector<airshot::OcrPreparationProgress> cancelled_progress;
    const auto cancelled_result = airshot::prepare_ocr_dependencies(
        offline_options,
        [&cancelled_progress](const airshot::OcrPreparationProgress& progress) {
            cancelled_progress.push_back(progress);
        },
        preparation_stop.get_token());
    expect(
        cancelled_result.cancelled &&
            cancelled_result.status.state ==
                airshot::OcrDependencyState::cancelled &&
            cancelled_result.status.recommended_action ==
                airshot::OcrRecoveryAction::retry &&
            cancelled_result.status.retryable &&
            cancelled_progress.size() == 2 &&
            cancelled_progress.back().state ==
                airshot::OcrDependencyState::cancelled,
        L"first-use preparation cancellation is explicit and retryable before network I/O");

    const auto callback_isolation = airshot::prepare_ocr_dependencies(
        offline_options,
        [](const airshot::OcrPreparationProgress&) {
            throw std::runtime_error("observer failure");
        });
    expect(
        callback_isolation.status.state == offline_result.status.state,
        L"OCR progress observers cannot abort or alter preparation state");

    const std::filesystem::path invalid_root =
        airshot::rapid_ocr_dependency_directory();
    std::error_code repair_filesystem_error;
    std::filesystem::create_directories(
        invalid_root,
        repair_filesystem_error);
    const std::filesystem::path invalid_file = invalid_root / L"unexpected.bin";
    HANDLE invalid_handle = CreateFileW(
        invalid_file.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    const bool invalid_fixture_created =
        !repair_filesystem_error &&
        invalid_handle != INVALID_HANDLE_VALUE;
    if (invalid_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(invalid_handle);
    }
    expect(
        invalid_fixture_created,
        L"OCR repair test creates an invalid local dependency directory");
    if (invalid_fixture_created) {
        std::stop_source repair_stop;
        repair_stop.request_stop();
        const auto cancelled_repair = airshot::repair_ocr_dependencies(
            airshot::kOcrEngineRapidV5Fast,
            repair_stop.get_token());
        expect(
            !cancelled_repair.ok &&
                !cancelled_repair.changed &&
                std::filesystem::exists(invalid_root) &&
                cancelled_repair.error.find(L"取消") != std::wstring::npos,
            L"OCR repair cancellation leaves the invalid package untouched");

        const auto repaired = airshot::repair_ocr_dependencies(
            airshot::kOcrEngineRapidV5Fast);
        expect(
            repaired.ok && repaired.changed &&
                !repaired.preserved_path.empty() &&
                !std::filesystem::exists(invalid_root) &&
                std::filesystem::exists(repaired.preserved_path) &&
                repaired.status.state !=
                    airshot::OcrDependencyState::repair_required,
            L"OCR repair quarantines invalid user-local data without deleting it");
    }

    if (old_size > 0) {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", old_value.c_str());
    } else {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr);
    }
}

void test_preparation_failure_classification() {
    const auto network =
        airshot::ocr_test_support::classify_preparation_failure(
            L"下载 OCR 依赖清单或签名失败：无法连接 OCR 依赖服务器");
    expect(
        network.state == airshot::OcrDependencyState::retryable_error &&
            network.recommended_action == airshot::OcrRecoveryAction::retry &&
            network.retryable && network.requires_network &&
            !network.security_blocked,
        L"OCR manifest transport failure remains retryable rather than looking like a signature attack");

    const auto local_failure =
        airshot::ocr_test_support::classify_preparation_failure(
            L"无法创建 OCR 组件隔离目录：访问被拒绝");
    expect(
        local_failure.state == airshot::OcrDependencyState::retryable_error &&
            local_failure.recommended_action ==
                airshot::OcrRecoveryAction::retry &&
            local_failure.retryable && !local_failure.requires_network &&
            !local_failure.security_blocked,
        L"local OCR preparation failures remain retryable without misleading network guidance");

    const auto local_lock_timeout =
        airshot::ocr_test_support::classify_preparation_failure(
            L"等待 OCR 状态互斥锁超时。");
    expect(
        local_lock_timeout.state ==
                airshot::OcrDependencyState::retryable_error &&
            local_lock_timeout.recommended_action ==
                airshot::OcrRecoveryAction::retry &&
            local_lock_timeout.retryable &&
            !local_lock_timeout.requires_network &&
            !local_lock_timeout.security_blocked,
        L"local OCR lock contention never produces misleading network guidance");

    const auto unsafe_redirect =
        airshot::ocr_test_support::classify_preparation_failure(
            L"下载 OCR 依赖清单或签名失败：OCR 下载重定向目标不是可信的 HTTPS URL。");
    expect(
        unsafe_redirect.state == airshot::OcrDependencyState::unavailable &&
            unsafe_redirect.recommended_action ==
                airshot::OcrRecoveryAction::contact_support &&
            unsafe_redirect.security_blocked &&
            !unsafe_redirect.retryable &&
            !unsafe_redirect.requires_network,
        L"OCR redirect downgrade failures remain security-blocked despite the transport wrapper");

    const auto invalid_clock =
        airshot::ocr_test_support::classify_preparation_failure(
            L"系统时间无效，无法验证 OCR 清单有效期。");
    expect(
        invalid_clock.state == airshot::OcrDependencyState::unavailable &&
            invalid_clock.recommended_action ==
                airshot::OcrRecoveryAction::check_system_time &&
            invalid_clock.security_blocked && invalid_clock.retryable,
        L"OCR clock validation failures provide an actionable recovery state");

    const auto signature =
        airshot::ocr_test_support::classify_preparation_failure(
            L"OCR 依赖清单 ECDSA P-256 签名无效。");
    expect(
        signature.state == airshot::OcrDependencyState::unavailable &&
            signature.recommended_action ==
                airshot::OcrRecoveryAction::contact_support &&
            signature.security_blocked && !signature.retryable,
        L"OCR cryptographic failure remains fail-closed and non-retryable");

    const auto installed =
        airshot::ocr_test_support::classify_preparation_failure(
            L"OCR 组件安装后状态复核失败：文件大小不一致");
    expect(
        installed.state == airshot::OcrDependencyState::repair_required &&
            installed.recommended_action == airshot::OcrRecoveryAction::repair &&
            installed.security_blocked && installed.retryable,
        L"OCR post-install verification failure recommends the explicit repair path");
}

void test_compiled_sequence_floor() {
    constexpr std::uint64_t expected =
        static_cast<std::uint64_t>(
            AIRSHOT_VERSION_MAJOR) *
            1'000'000'000'000ULL +
        static_cast<std::uint64_t>(
            AIRSHOT_VERSION_MINOR) *
            1'000'000ULL +
        static_cast<std::uint64_t>(
            AIRSHOT_VERSION_PATCH);
    const std::uint64_t compiled =
        airshot::ocr_minimum_sequence();
    expect(
        compiled ==
            static_cast<std::uint64_t>(
                AIRSHOT_OCR_MIN_SEQUENCE),
        L"OCR runtime sequence floor matches compile definition");
#if AIRSHOT_OCR_MIN_SEQUENCE_IS_DEFAULT
    expect(
        compiled == expected,
        L"OCR default sequence floor derives from VERSION");
#else
    static_cast<void>(expected);
#endif
    expect(
        !airshot::ocr_test_support::
            sequence_is_allowed(
                compiled - 1,
                0),
        L"OCR sequence policy rejects below compiled floor");
    expect(
        airshot::ocr_test_support::
            sequence_is_allowed(
                compiled,
                0),
        L"OCR sequence policy accepts compiled floor");
    expect(
        !airshot::ocr_test_support::
            sequence_is_allowed(
                compiled,
                compiled + 1),
        L"OCR sequence policy rejects below persisted floor");
}

void test_sequence_high_watermark() {
    using airshot::ocr_test_support::
        parse_sequence;
    expect(
        parse_sequence("1") == 1,
        L"OCR sequence parser accepts one");
    expect(
        parse_sequence(
            "9007199254740991") ==
            9'007'199'254'740'991ULL,
        L"OCR sequence parser accepts maximum safe integer");
    for (const std::string_view invalid :
         {"", "0", "01", "+1", "1\n",
          "9007199254740992"}) {
        expect(
            !parse_sequence(invalid),
            L"OCR sequence parser rejects non-canonical input");
    }

    ScopedTestDirectory directory;
    expect(
        !directory.path().empty(),
        L"create OCR sequence test directory");
    if (directory.path().empty()) {
        return;
    }
    const auto state =
        directory.path() / L"sequence";
    std::wstring error;
    const auto missing =
        airshot::ocr_test_support::
            read_sequence_file(
                state,
                true,
                &error);
    expect(
        missing && *missing == 0,
        L"OCR missing sequence state starts at zero");
    expect(
        airshot::ocr_test_support::
            update_sequence_file(
                state,
                42,
                &error),
        L"OCR sequence state writes atomically with private ACL");
    const auto written =
        airshot::ocr_test_support::
            read_sequence_file(
                state,
                false,
                &error);
    expect(
        written && *written == 42,
        L"OCR sequence state round-trips");
    expect(
        !airshot::ocr_test_support::
            update_sequence_file(
                state,
                41,
                &error),
        L"OCR sequence state rejects rollback");
    const auto unchanged =
        airshot::ocr_test_support::
            read_sequence_file(
                state,
                false,
                &error);
    expect(
        unchanged && *unchanged == 42,
        L"OCR rejected rollback preserves high watermark");

    ScopedHandle corrupt_file(CreateFileW(
        state.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        TRUNCATE_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    expect(
        corrupt_file.get() &&
            corrupt_file.get() !=
                INVALID_HANDLE_VALUE,
        L"open OCR sequence state for corruption test");
    if (corrupt_file.get() &&
        corrupt_file.get() !=
            INVALID_HANDLE_VALUE) {
        constexpr char corrupt[] = "01";
        DWORD written_bytes = 0;
        expect(
            WriteFile(
                corrupt_file.get(),
                corrupt,
                2,
                &written_bytes,
                nullptr) &&
                written_bytes == 2 &&
                FlushFileBuffers(
                    corrupt_file.get()),
            L"write corrupt OCR sequence state");
        corrupt_file.reset();
        expect(
            !airshot::ocr_test_support::
                read_sequence_file(
                    state,
                    false,
                    &error),
            L"OCR corrupt sequence state fails closed");
    }
}

void test_locked_path_share_contract() {
    ScopedTestDirectory directory;
    expect(
        !directory.path().empty(),
        L"create OCR lease test directory");
    if (directory.path().empty()) {
        return;
    }
    const auto file =
        directory.path() / L"model.onnx";
    {
        ScopedHandle creator(CreateFileW(
            file.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        constexpr char content[] = "model";
        DWORD written = 0;
        expect(
            creator.get() &&
                creator.get() !=
                    INVALID_HANDLE_VALUE &&
                WriteFile(
                    creator.get(),
                    content,
                    5,
                    &written,
                    nullptr) &&
                written == 5,
            L"create OCR lease test file");
    }

    std::wstring error;
    ScopedHandle guard(
        airshot::ocr_test_support::lock_path(
            file,
            false,
            &error));
    expect(
        guard.get() &&
            guard.get() != INVALID_HANDLE_VALUE,
        L"OCR production guard locks file");
    if (!guard.get() ||
        guard.get() == INVALID_HANDLE_VALUE) {
        return;
    }

    ScopedHandle read_only(CreateFileW(
        file.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    expect(
        read_only.get() &&
            read_only.get() !=
                INVALID_HANDLE_VALUE,
        L"OCR guard permits read-only loader access");
    read_only.reset();

    SetLastError(ERROR_SUCCESS);
    ScopedHandle writer(CreateFileW(
        file.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ |
            FILE_SHARE_WRITE |
            FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    expect(
        !writer.get() ||
            writer.get() == INVALID_HANDLE_VALUE,
        L"OCR guard blocks writer while lease is live");
    expect(
        GetLastError() ==
            ERROR_SHARING_VIOLATION,
        L"OCR blocked writer reports sharing violation");
    expect(
        !DeleteFileW(file.c_str()),
        L"OCR guard blocks delete while lease is live");

    guard.reset();
    writer.reset(CreateFileW(
        file.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    expect(
        writer.get() &&
            writer.get() !=
                INVALID_HANDLE_VALUE,
        L"OCR writer succeeds after lease release");
    writer.reset();
    expect(
        DeleteFileW(file.c_str()) != FALSE,
        L"OCR delete succeeds after lease release");
}

void test_dependency_hash_cache_and_cancellation() {
    const std::filesystem::path dependency_root(L"C:\\ocr-deps");
    constexpr std::wstring_view relative_path =
        L"models/rapidocr-v5-fast/det.onnx";
    constexpr std::wstring_view digest =
        L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    expect(
        airshot::ocr_test_support::dependency_hash_cache_key_matches(
            true,
            dependency_root,
            99,
            relative_path,
            digest,
            5,
            std::filesystem::path(L"c:\\OCR-DEPS"),
            99,
            L"MODELS/RAPIDOCR-V5-FAST/DET.ONNX",
            L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            5),
        L"OCR dependency hash cache reuses the same locked file identity");
    expect(
        !airshot::ocr_test_support::dependency_hash_cache_key_matches(
            false,
            dependency_root,
            99,
            relative_path,
            digest,
            5,
            dependency_root,
            99,
            relative_path,
            digest,
            5),
        L"OCR dependency hash cache rejects a replaced file object");
    expect(
        !airshot::ocr_test_support::dependency_hash_cache_key_matches(
            true,
            dependency_root,
            99,
            relative_path,
            digest,
            5,
            dependency_root,
            100,
            relative_path,
            digest,
            5) &&
            !airshot::ocr_test_support::dependency_hash_cache_key_matches(
                true,
                dependency_root,
                99,
                relative_path,
                digest,
                5,
                dependency_root,
                99,
                relative_path,
                L"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
                5) &&
            !airshot::ocr_test_support::dependency_hash_cache_key_matches(
                true,
                dependency_root,
                99,
                relative_path,
                digest,
                5,
                dependency_root,
                99,
                relative_path,
                digest,
                6),
        L"OCR dependency hash cache invalidates sequence, digest, and size changes");

    ScopedTestDirectory directory;
    expect(
        !directory.path().empty(),
        L"create OCR hash cancellation test directory");
    if (directory.path().empty()) {
        return;
    }
    const auto file = directory.path() / L"model.onnx";
    {
        ScopedHandle creator(CreateFileW(
            file.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        constexpr char content[] = "model";
        DWORD written = 0;
        expect(
            creator.get() &&
                creator.get() != INVALID_HANDLE_VALUE &&
                WriteFile(
                    creator.get(),
                    content,
                    5,
                    &written,
                    nullptr) &&
                written == 5,
            L"create OCR hash cancellation test file");
    }

    std::wstring error;
    const auto digest_result =
        airshot::ocr_test_support::sha256_file(
            file,
            {},
            &error);
    expect(
        digest_result && digest_result->size() == 64,
        L"OCR dependency hashing succeeds for a locked file");

    std::stop_source cancellation;
    cancellation.request_stop();
    error.clear();
    expect(
        !airshot::ocr_test_support::sha256_file(
            file,
            cancellation.get_token(),
            &error) &&
            error.find(L"取消") != std::wstring::npos,
        L"OCR dependency hashing honors pre-requested cancellation");
    error.clear();
    expect(
        !airshot::acquire_ocr_dependency_lease(
            directory.path() / L"missing-package",
            true,
            false,
            &error,
            cancellation.get_token()) &&
            error.find(L"取消") != std::wstring::npos,
        L"OCR dependency lease acquisition honors cancellation before I/O");
}

void test_manifest_verifier_cli_contract() {
    const std::array incomplete{
        std::wstring(L"--verify-ocr-manifest"),
        std::wstring(L"manifest.json"),
    };
    expect(
        airshot::run_ocr_manifest_verifier(incomplete) ==
            static_cast<int>(airshot::ExitCode::invalid_arguments),
        L"OCR manifest verifier rejects incomplete arguments");

    const std::array missing_files{
        std::wstring(L"--verify-ocr-manifest"),
        std::wstring(L"missing-manifest.json"),
        std::wstring(L"missing-manifest.json.sig"),
    };
    expect(
        airshot::run_ocr_manifest_verifier(missing_files) ==
            static_cast<int>(airshot::ExitCode::operation_failed),
        L"OCR manifest verifier fails closed for unreadable input");
}

std::string ocr_protocol_json(std::string_view blocks) {
    std::string json =
        R"({"schemaVersion":1,"profile":"rapidocr-v5-fast","preprocess":{"sourceWidth":100,"sourceHeight":50,"inputWidth":200,"inputHeight":100,"scaleX":2,"scaleY":2,"resample":"bilinear-upscale","tiled":false,"tileCount":1,"tileSize":0,"tileOverlap":0,"coordinateSpace":"input-pixels"},"timings":{"decodeMs":1,"modelInitMs":2,"inferenceMs":3,"mergeMs":0.5,"totalMs":7},"blocks":[)";
    json += blocks;
    json += R"(]})";
    return json;
}

airshot::OcrProtocolExpectations protocol_expectations() {
    return {
        airshot::kOcrEngineRapidV5Fast,
        100,
        50,
        200,
        100,
        2.0,
        2.0,
        L"bilinear-upscale",
    };
}

void test_ocr_protocol_parser_and_ordering() {
    const std::string protocol = ocr_protocol_json(
        R"({"quad":[[120,12],[180,12],[180,32],[120,32]],"text":"B","score":0.8},{"quad":[[20,60],[80,60],[80,80],[20,80]],"text":"C","score":0.7},{"quad":[[10,10],[90,10],[90,30],[10,30]],"text":"A","score":0.9})");
    std::wstring error;
    const auto parsed = airshot::parse_ocr_runner_protocol(
        protocol,
        protocol_expectations(),
        &error);
    expect(parsed.has_value(), L"OCR protocol accepts valid structured output");
    if (!parsed) {
        return;
    }
    expect(parsed->ok, L"OCR protocol reports recognized blocks as success");
    expect(parsed->text == L"A B\r\nC", L"OCR protocol applies deterministic row then column ordering");
    expect(parsed->blocks.size() == 3, L"OCR protocol retains structured blocks");
    expect(
        parsed->blocks.size() == 3 && parsed->blocks[0].text == L"A" &&
            parsed->blocks[1].text == L"B" && parsed->blocks[2].text == L"C",
        L"OCR protocol returns blocks in generated text order");
    expect(
        parsed->blocks.size() == 3 &&
            std::abs(parsed->blocks[0].quad[0].x - 5.0) < 0.001 &&
            std::abs(parsed->blocks[0].quad[0].y - 5.0) < 0.001,
        L"OCR protocol maps input pixel quads back to source pixels");
    expect(
        parsed->profile == airshot::kOcrEngineRapidV5Fast &&
            parsed->preprocess.input_width == 200 &&
            std::abs(parsed->timings.inference_ms - 3.0) < 0.001,
        L"OCR protocol retains profile preprocessing and timings");

    const auto empty = airshot::parse_ocr_runner_protocol(
        ocr_protocol_json(""),
        protocol_expectations(),
        &error);
    expect(
        empty.has_value() && !empty->ok && empty->blocks.empty() && !empty->error.empty(),
        L"OCR protocol represents a valid no-text result without malformed output");
}

void test_ocr_protocol_rejects_untrusted_fields() {
    std::wstring error;
    const airshot::OcrProtocolExpectations expected = protocol_expectations();
    const std::string invalid_utf8(1, static_cast<char>(0xff));
    expect(
        !airshot::parse_ocr_runner_protocol(invalid_utf8, expected, &error),
        L"OCR protocol rejects invalid UTF-8");

    std::string unknown_field = ocr_protocol_json("");
    unknown_field.insert(unknown_field.size() - 1, R"(,"unexpected":true)");
    expect(
        !airshot::parse_ocr_runner_protocol(unknown_field, expected, &error),
        L"OCR protocol rejects unknown top-level fields");

    std::string duplicate_field = ocr_protocol_json("");
    duplicate_field.insert(1, R"("profile":"rapidocr-v5-fast",)");
    expect(
        !airshot::parse_ocr_runner_protocol(duplicate_field, expected, &error),
        L"OCR protocol rejects duplicate object fields");

    expect(
        !airshot::parse_ocr_runner_protocol(
            ocr_protocol_json(
                R"({"quad":[[0,0],[201,0],[201,10],[0,10]],"text":"x","score":0.8})"),
            expected,
            &error),
        L"OCR protocol rejects out of bounds quad coordinates");
    expect(
        !airshot::parse_ocr_runner_protocol(
            ocr_protocol_json(
                R"({"quad":[[0,0],[10,0],[10,10],[0,10]],"text":"x","score":1e309})"),
            expected,
            &error),
        L"OCR protocol rejects non-finite confidence values");
    expect(
        !airshot::parse_ocr_runner_protocol(
            ocr_protocol_json(
                R"({"quad":[[0,0],[10,0],[10,10],[0,10]],"text":"x\nwarning","score":0.8})"),
            expected,
            &error),
        L"OCR protocol rejects control characters in block text");

    const std::string oversized_text = std::string(4'097, 'a');
    expect(
        !airshot::parse_ocr_runner_protocol(
            ocr_protocol_json(
                R"({"quad":[[0,0],[10,0],[10,10],[0,10]],"text":")" +
                oversized_text + R"(","score":0.8})"),
            expected,
            &error),
        L"OCR protocol rejects oversized block text");

    std::string excessive_blocks;
    constexpr std::string_view block =
        R"({"quad":[[0,0],[1,0],[1,1],[0,1]],"text":"x","score":1})";
    for (std::size_t index = 0; index < 16'385; ++index) {
        if (!excessive_blocks.empty()) {
            excessive_blocks.push_back(',');
        }
        excessive_blocks += block;
    }
    expect(
        !airshot::parse_ocr_runner_protocol(
            ocr_protocol_json(excessive_blocks),
            expected,
            &error),
        L"OCR protocol rejects excessive block counts");
}

void test_ocr_preprocess_scaling_and_threads() {
    airshot::Bitmap source(2, 2);
    const auto set_gray = [](airshot::Bitmap& bitmap, int x, int y, std::uint8_t value) {
        auto row = bitmap.row(y);
        const std::size_t offset = static_cast<std::size_t>(x) * airshot::Bitmap::bytes_per_pixel;
        row[offset] = value;
        row[offset + 1] = value;
        row[offset + 2] = value;
        row[offset + 3] = 255;
    };
    set_gray(source, 0, 0, 0);
    set_gray(source, 1, 0, 100);
    set_gray(source, 0, 1, 200);
    set_gray(source, 1, 1, 255);
    const airshot::Bitmap enlarged =
        airshot::ocr_test_support::resize_bitmap_high_quality(source, 3, 3);
    expect(enlarged.valid(), L"OCR bilinear enlargement returns a valid bitmap");
    if (enlarged.valid()) {
        const auto center = enlarged.row(1);
        expect(
            center[4] >= 138 && center[4] <= 139 && center[7] == 255,
            L"OCR bilinear enlargement interpolates instead of nearest-neighbor sampling");
    }

    airshot::Bitmap checkerboard(8, 8);
    for (int y = 0; y < checkerboard.height; ++y) {
        for (int x = 0; x < checkerboard.width; ++x) {
            set_gray(checkerboard, x, y, (x + y) % 2 == 0 ? 0 : 255);
        }
    }
    const airshot::Bitmap reduced =
        airshot::ocr_test_support::resize_bitmap_high_quality(checkerboard, 1, 1);
    expect(
        reduced.valid() && reduced.pixels[0] >= 120 && reduced.pixels[0] <= 136,
        L"OCR progressive bilinear downsampling preserves mixed high-frequency content");

    expect(
        std::abs(airshot::ocr_test_support::select_preprocess_scale(800, 600) - 2.0) < 0.001,
        L"OCR bounded preprocessing uses 2x for small captures");
    expect(
        std::abs(airshot::ocr_test_support::select_preprocess_scale(1400, 900) - 1.5) < 0.001,
        L"OCR bounded preprocessing uses 1.5x for medium captures");
    expect(
        std::abs(airshot::ocr_test_support::select_preprocess_scale(1920, 1080) - 1.0) < 0.001,
        L"OCR preprocessing leaves normal full-HD captures unchanged");
    expect(
        std::abs(airshot::ocr_test_support::select_preprocess_scale(1440, 10'000) - 1.0) < 0.001,
        L"OCR preprocessing preserves long screenshots for runner tiling");
    expect(
        airshot::ocr_test_support::select_preprocess_scale(10'000, 10'000) < 1.0,
        L"OCR preprocessing bounds exceptionally large pixel surfaces");

    expect(
        airshot::ocr_test_support::select_thread_count(5'000'000, 1, true) == 1,
        L"OCR thread selection respects single-core systems");
    expect(
        airshot::ocr_test_support::select_thread_count(200'000, 16, false) == 2,
        L"OCR thread selection limits small images");
    expect(
        airshot::ocr_test_support::select_thread_count(3'000'000, 16, false) == 3,
        L"OCR thread selection scales normal large images to three threads");
    expect(
        airshot::ocr_test_support::select_thread_count(5'000'000, 16, true) == 4,
        L"OCR thread selection permits four threads for accurate large images");
}

std::string warm_worker_response_json(
    std::uint64_t request_id,
    std::string_view dependency_root,
    std::uint64_t sequence,
    std::string_view profile,
    std::string_view result) {
    return std::string(R"({"schemaVersion":1,"requestId":)") +
           std::to_string(request_id) + R"(,"profile":")" +
           std::string(profile) + R"(","dependencyRoot":")" +
           std::string(dependency_root) + R"(","dependencySequence":)" +
           std::to_string(sequence) + R"(,"ok":true,"result":)" +
           std::string(result) + "}";
}

void test_warm_worker_protocol_and_state_machine() {
    const auto expected_image = protocol_expectations();
    const std::filesystem::path dependency_root(L"C:\\ocr-deps");
    const std::string nested_result = ocr_protocol_json(
        R"({"quad":[[10,10],[90,10],[90,30],[10,30]],"text":"warm","score":0.9})");
    const std::string valid = warm_worker_response_json(
        42,
        R"(C:\\ocr-deps)",
        99,
        "rapidocr-v5-fast",
        nested_result);
    std::wstring error;
    const auto parsed = airshot::ocr_test_support::parse_warm_worker_response(
        valid,
        42,
        airshot::kOcrEngineRapidV5Fast,
        dependency_root,
        99,
        expected_image,
        &error);
    expect(
        parsed.has_value() && parsed->ok && parsed->output.ok &&
            parsed->output.text == L"warm",
        L"warm OCR response binds and parses the nested Stage 1 protocol");

    expect(
        !airshot::ocr_test_support::parse_warm_worker_response(
            valid,
            43,
            airshot::kOcrEngineRapidV5Fast,
            dependency_root,
            99,
            expected_image,
            &error),
        L"warm OCR response rejects stale request ids");
    expect(
        !airshot::ocr_test_support::parse_warm_worker_response(
            valid,
            42,
            airshot::kOcrEngineRapidV5Accurate,
            dependency_root,
            99,
            expected_image,
            &error),
        L"warm OCR response rejects profile changes");
    expect(
        !airshot::ocr_test_support::parse_warm_worker_response(
            valid,
            42,
            airshot::kOcrEngineRapidV5Fast,
            dependency_root,
            100,
            expected_image,
            &error),
        L"warm OCR response rejects dependency sequence changes");
    expect(
        !airshot::ocr_test_support::parse_warm_worker_response(
            valid,
            42,
            airshot::kOcrEngineRapidV5Fast,
            std::filesystem::path(L"C:\\other-deps"),
            99,
            expected_image,
            &error),
        L"warm OCR response rejects dependency root changes");

    std::string wrong_image = valid;
    const std::string from = R"("inputWidth":200)";
    const std::string to = R"("inputWidth":199)";
    const std::size_t image_position = wrong_image.find(from);
    expect(image_position != std::string::npos, L"warm OCR image fixture contains width");
    if (image_position != std::string::npos) {
        wrong_image.replace(image_position, from.size(), to);
        expect(
            !airshot::ocr_test_support::parse_warm_worker_response(
                wrong_image,
                42,
                airshot::kOcrEngineRapidV5Fast,
                dependency_root,
                99,
                expected_image,
                &error),
            L"warm OCR response rejects mismatched image metadata");
    }

    expect(
        airshot::ocr_test_support::warm_worker_key_matches(
            true,
            dependency_root,
            99,
            airshot::kOcrEngineRapidV5Fast,
            2,
            std::filesystem::path(L"c:\\OCR-DEPS"),
            99,
            airshot::kOcrEngineRapidV5Fast,
            2),
        L"warm OCR reuses a healthy matching dependency/profile key");
    expect(
        !airshot::ocr_test_support::warm_worker_key_matches(
            false,
            dependency_root,
            99,
            airshot::kOcrEngineRapidV5Fast,
            2,
            dependency_root,
            99,
            airshot::kOcrEngineRapidV5Fast,
            2),
        L"warm OCR discards a crashed process");
    expect(
        !airshot::ocr_test_support::warm_worker_key_matches(
            true,
            dependency_root,
            99,
            airshot::kOcrEngineRapidV5Fast,
            2,
            dependency_root,
            100,
            airshot::kOcrEngineRapidV5Fast,
            2),
        L"warm OCR restarts for dependency version changes");
    expect(
        !airshot::ocr_test_support::warm_worker_key_matches(
            true,
            dependency_root,
            99,
            airshot::kOcrEngineRapidV5Fast,
            2,
            dependency_root,
            99,
            airshot::kOcrEngineRapidV5Fast,
            4),
        L"warm OCR restarts when thread selection grows from 2 to 4");
    expect(
        !airshot::ocr_test_support::warm_worker_key_matches(
            true,
            dependency_root,
            99,
            airshot::kOcrEngineRapidV5Fast,
            4,
            dependency_root,
            99,
            airshot::kOcrEngineRapidV5Fast,
            2),
        L"warm OCR restarts when thread selection shrinks from 4 to 2");
    expect(
        airshot::ocr_test_support::warm_failure_allows_fallback(false, false),
        L"warm OCR non-cancellation failure permits Stage 1 fallback");
    expect(
        !airshot::ocr_test_support::warm_failure_allows_fallback(true, false) &&
            !airshot::ocr_test_support::warm_failure_allows_fallback(false, true),
        L"warm OCR cancellation never triggers cold fallback");
}

void test_warm_worker_frame_limits() {
    const std::array<char, 4> valid_header{
        static_cast<char>(0x34), static_cast<char>(0x12), 0, 0};
    const auto valid = airshot::ocr_test_support::decode_warm_frame_size(
        valid_header,
        64 * 1024);
    expect(valid && *valid == 0x1234, L"warm OCR frame decodes little-endian length");
    const std::array<char, 4> zero_header{};
    expect(
        !airshot::ocr_test_support::decode_warm_frame_size(zero_header, 64 * 1024),
        L"warm OCR frame rejects zero length");
    const std::array<char, 4> oversized_header{0, 0, 2, 0};
    expect(
        !airshot::ocr_test_support::decode_warm_frame_size(
            oversized_header,
            64 * 1024),
        L"warm OCR frame rejects lengths above the configured bound");
    expect(
        !airshot::ocr_test_support::decode_warm_frame_size(
            std::span<const char>(valid_header).first(3),
            64 * 1024),
        L"warm OCR frame rejects truncated headers");
}

void test_pre_requested_recognition_cancellation() {
    airshot::Bitmap bitmap(1, 1);
    airshot::AppConfig config;
    std::stop_source cancellation;
    cancellation.request_stop();
    const airshot::OcrOutput output =
        airshot::recognize_text(
            bitmap,
            config,
            cancellation.get_token());
    expect(
        !output.ok &&
            output.error.find(L"取消") !=
                std::wstring::npos,
        L"OCR recognition honors pre-requested cancellation");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 &&
        std::wstring_view(argv[1]) ==
            L"--verify-ocr-manifest") {
        const std::array arguments{
            std::wstring(argv[1]),
            std::wstring(argv[2]),
            std::wstring(argv[3]),
        };
        return airshot::run_ocr_manifest_verifier(
            arguments);
    }
    if (argc == 3 &&
        std::wstring_view(argv[1]) ==
            L"--lease-ocr-package") {
        std::wstring error;
        const auto lease =
            airshot::acquire_ocr_dependency_lease(
                std::filesystem::path(argv[2]),
                true,
                false,
                &error);
        if (!lease) {
            std::wcerr << error << L"\n";
            return 5;
        }
        return 0;
    }

    test_manifest_parser();
    test_signature_parser();
    test_signature_verifier();
    test_pre_requested_download_cancellation();
    test_unconfigured_build_is_offline_and_unavailable();
    test_preparation_failure_classification();
    test_compiled_sequence_floor();
    test_sequence_high_watermark();
    test_locked_path_share_contract();
    test_dependency_hash_cache_and_cancellation();
    test_manifest_verifier_cli_contract();
    test_ocr_protocol_parser_and_ordering();
    test_ocr_protocol_rejects_untrusted_fields();
    test_ocr_preprocess_scaling_and_threads();
    test_warm_worker_protocol_and_state_machine();
    test_warm_worker_frame_limits();
    test_pre_requested_recognition_cancellation();
    if (failures == 0) {
        std::wcout << L"All OCR tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
