#include "airshot/ocr.h"
#include "ocr_test_support.h"

#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stop_token>
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
    std::vector<std::wstring> paths{L"rapidocr_runner.exe"};
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
        valid && valid->files.size() == 13,
        L"OCR manifest accepts runner-only required payload");

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
    test_compiled_sequence_floor();
    test_sequence_high_watermark();
    test_locked_path_share_contract();
    test_manifest_verifier_cli_contract();
    if (failures == 0) {
        std::wcout << L"All OCR tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
