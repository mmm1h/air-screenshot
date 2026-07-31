#include "airshot/portable.h"
#include "portable_internal.h"

#include <sddl.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <latch>
#include <limits>
#include <thread>

namespace {

int failures = 0;

void expect(bool condition, std::wstring_view message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
        ++failures;
    }
}

std::wstring manifest_json(
    std::wstring_view version = L"1.2.3",
    std::wstring_view url = L"https://example.com/AirScreenshot.exe",
    std::wstring_view hash =
        L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    std::wstring_view size = L"1024") {
    return std::format(
        LR"({{"version":"{}","url":"{}","sha256":"{}","size":{}}})",
        version,
        url,
        hash,
        size);
}

void test_versions() {
    expect(airshot::version_is_newer(L"0.2.3", L"0.2.4"),
           L"patch version advances");
    expect(airshot::version_is_newer(L"0.9.9", L"1.0.0"),
           L"major version advances");
    expect(!airshot::version_is_newer(L"1.0.0", L"1.0.0"),
           L"equal versions are not newer");
    expect(!airshot::version_is_newer(L"1.0.0", L"01.0.1"),
           L"noncanonical leading zero is rejected");
    expect(!airshot::version_is_newer(L"1.0", L"1.0.1"),
           L"incomplete current version is rejected");
    expect(!airshot::version_is_newer(L"1.0.0", L"1.0.0.1"),
           L"four-part candidate is rejected");
    expect(!airshot::version_is_newer(L"1.0.0", L"65536.0.0"),
           L"version resource overflow is rejected");
    expect(!airshot::version_is_newer(
               L"1.0.0", L"184467440737095516160.0.0"),
           L"numeric overflow is rejected");
}

void test_manifests() {
    const auto parsed = airshot::parse_update_manifest(manifest_json());
    expect(parsed && parsed->version == L"1.2.3" &&
               parsed->url == L"https://example.com/AirScreenshot.exe" &&
               parsed->sha256 ==
                   L"0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF" &&
               parsed->size == 1024,
           L"valid update manifest parses and normalizes its hash");

    if (parsed) {
        const auto round_trip =
            airshot::parse_update_manifest(airshot::update_manifest_to_json(*parsed));
        expect(round_trip && round_trip->version == parsed->version &&
                   round_trip->url == parsed->url &&
                   round_trip->sha256 == parsed->sha256 &&
                   round_trip->size == parsed->size,
               L"update manifest round trip is stable");
    }

    expect(!airshot::parse_update_manifest(
               manifest_json(L"1.2.3", L"http://example.com/AirScreenshot.exe")),
           L"plain HTTP is rejected");
    expect(!airshot::parse_update_manifest(
               manifest_json(
                   L"1.2.3",
                   L"https://user:password@example.com/AirScreenshot.exe")),
           L"URL credentials are rejected");
    expect(!airshot::parse_update_manifest(
               manifest_json(
                   L"1.2.3",
                   L"https://example.com/AirScreenshot.exe#fragment")),
           L"URL fragments are rejected");
    expect(!airshot::parse_update_manifest(
               manifest_json(
                   L"1.2.3",
                   LR"(https:\\example.com\AirScreenshot.exe)")),
           L"backslash URL confusion is rejected");
    expect(!airshot::parse_update_manifest(manifest_json(
               L"1.2.3",
               L"https://example.com/AirScreenshot.exe",
               L"0000000000000000000000000000000000000000000000000000000000000000")),
           L"all-zero executable hash is rejected");
    expect(!airshot::parse_update_manifest(
               manifest_json(
                   L"1.2.3",
                   L"https://example.com/AirScreenshot.exe",
                   L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                   L"1.5")),
           L"fractional executable size is rejected");
    expect(!airshot::parse_update_manifest(
               manifest_json(
                   L"1.2.3",
                   L"https://example.com/AirScreenshot.exe",
                   L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                   std::to_wstring(airshot::kMaxPortableUpdateBytes + 1))),
           L"oversized executable is rejected");
    expect(airshot::parse_update_manifest(
               manifest_json(
                   L"65535.65535.65535",
                   L"https://example.com/AirScreenshot.exe",
                   L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                   std::to_wstring(airshot::kMaxPortableUpdateBytes)))
               .has_value(),
           L"documented size and version limits are accepted");
}

void test_pending_update_sources() {
    const auto manifest = airshot::parse_update_manifest(manifest_json());
    expect(manifest.has_value(),
           L"pending update source tests have a valid manifest");
    if (!manifest) {
        return;
    }

    const auto legacy_source =
        airshot::portable_internal::pending_update_source_from_json(
            manifest_json());
    expect(legacy_source == airshot::UpdateRequestSource::automatic,
           L"legacy pending metadata is conservatively treated as automatic");

    const std::wstring automatic_json =
        airshot::portable_internal::pending_update_to_json_for_testing(
            *manifest, airshot::UpdateRequestSource::automatic);
    const std::wstring manual_json =
        airshot::portable_internal::pending_update_to_json_for_testing(
            *manifest, airshot::UpdateRequestSource::manual);
    expect(
        airshot::portable_internal::pending_update_source_from_json(
            automatic_json) == airshot::UpdateRequestSource::automatic,
        L"automatic pending update source round trips");
    expect(
        airshot::portable_internal::pending_update_source_from_json(
            manual_json) == airshot::UpdateRequestSource::manual,
        L"manual pending update source round trips");

    std::wstring unknown_json = automatic_json;
    const std::wstring automatic_marker = L"\"automatic\"";
    const std::size_t marker = unknown_json.find(automatic_marker);
    if (marker != std::wstring::npos) {
        unknown_json.replace(marker, automatic_marker.size(), L"\"unknown\"");
    }
    expect(
        marker != std::wstring::npos &&
            !airshot::portable_internal::pending_update_source_from_json(
                unknown_json),
        L"unknown pending update sources are rejected");

    expect(
        !airshot::portable_internal::pending_update_source_allowed(
            airshot::UpdateRequestSource::automatic, false) &&
            airshot::portable_internal::pending_update_source_allowed(
                airshot::UpdateRequestSource::manual, false) &&
            airshot::portable_internal::pending_update_source_allowed(
                airshot::UpdateRequestSource::automatic, true),
        L"disabled automatic updates pause only automatically staged payloads");
}

void test_mark_pending_update_manual_on_disk() {
    const auto manifest = airshot::parse_update_manifest(
        manifest_json(L"65535.65535.65535"));
    expect(manifest.has_value(),
           L"manual pending promotion has a newer valid manifest");
    if (!manifest) {
        return;
    }

    const auto root = std::filesystem::temp_directory_path() /
                      std::format(
                          L"airshot-portable-manual-intent-test-{}-{}",
                          GetCurrentProcessId(),
                          GetTickCount64());
    const DWORD old_size =
        GetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr, 0);
    std::wstring old_value(old_size, L'\0');
    if (old_size > 0) {
        const DWORD copied = GetEnvironmentVariableW(
            L"AIRSHOT_DATA_DIR", old_value.data(), old_size);
        old_value.resize(copied);
    }
    SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", root.c_str());

    const auto updates = root / L"updates";
    const auto pending_path = updates / L"pending.json";
    std::filesystem::create_directories(updates);
    {
        std::ofstream stream(
            pending_path, std::ios::binary | std::ios::trunc);
        const std::string json = airshot::to_utf8(
            airshot::portable_internal::pending_update_to_json_for_testing(
                *manifest, airshot::UpdateRequestSource::automatic));
        stream.write(
            json.data(), static_cast<std::streamsize>(json.size()));
    }

    std::wstring error = L"stale";
    expect(airshot::mark_pending_update_manual(&error) ==
                   airshot::PendingUpdateManualResult::ready &&
               error.empty(),
           L"an automatically staged update is promoted to manual intent on disk");

    std::string promoted_bytes;
    {
        std::ifstream stream(pending_path, std::ios::binary);
        stream.seekg(0, std::ios::end);
        const auto size = stream.tellg();
        if (size > 0) {
            promoted_bytes.resize(static_cast<std::size_t>(size));
            stream.seekg(0, std::ios::beg);
            stream.read(
                promoted_bytes.data(),
                static_cast<std::streamsize>(promoted_bytes.size()));
        }
    }
    expect(
        airshot::portable_internal::pending_update_source_from_json(
            airshot::from_utf8(promoted_bytes)) ==
            airshot::UpdateRequestSource::manual,
        L"manual update intent survives a fresh pending.json read");

    error = L"stale";
    expect(airshot::mark_pending_update_manual(&error) ==
                   airshot::PendingUpdateManualResult::ready &&
               error.empty(),
           L"manual pending promotion is idempotent");

    std::error_code ignored;
    std::filesystem::remove(pending_path, ignored);
    error = L"stale";
    expect(airshot::mark_pending_update_manual(&error) ==
                   airshot::PendingUpdateManualResult::missing &&
               error.empty(),
           L"a missing pending update is reported without a false error");

    {
        std::ofstream stream(
            pending_path, std::ios::binary | std::ios::trunc);
        stream << "{}";
    }
    error = L"stale";
    expect(airshot::mark_pending_update_manual(&error) ==
                   airshot::PendingUpdateManualResult::invalid &&
               !error.empty(),
           L"invalid pending metadata is distinguished from a retryable promotion failure");

    if (old_size > 0) {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", old_value.c_str());
    } else {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr);
    }
    std::filesystem::remove_all(root, ignored);
}

void test_hashing() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::format(
                          L"airshot-portable-test-{}-{}",
                          GetCurrentProcessId(),
                          GetTickCount64());
    std::filesystem::create_directories(root);
    const auto file = root / L"abc.bin";
    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        stream << "abc";
    }

    std::wstring error = L"stale";
    expect(
        airshot::sha256_file(file, &error) ==
                L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD" &&
            error.empty(),
        L"SHA256 reads a regular file and clears stale errors");

    expect(airshot::sha256_file(root / L"missing.bin", &error).empty() &&
               !error.empty(),
           L"SHA256 reports a missing file");
    expect(airshot::sha256_file(root, &error).empty() && !error.empty(),
           L"SHA256 rejects a directory");

    airshot::UpdateManifest invalid;
    invalid.version = L"1.2.3";
    invalid.url = L"https://example.com/AirScreenshot.exe";
    invalid.sha256.assign(64, L'0');
    invalid.size = 3;
    expect(!airshot::verify_portable_executable(file, invalid, &error) &&
               !error.empty(),
           L"executable verification rejects an invalid manifest before trust checks");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_helper_boundary() {
    const std::array<std::wstring, 1> unknown{L"--unknown"};
    expect(airshot::run_update_helper(unknown) ==
               static_cast<int>(airshot::ExitCode::invalid_arguments),
           L"unknown helper command is rejected");

    const std::array<std::wstring, 4> invalid_mode{
        L"--apply-update", L"C:\\AirScreenshot.exe", L"1", L"maybe"};
    expect(airshot::run_update_helper(invalid_mode) ==
               static_cast<int>(airshot::ExitCode::invalid_arguments),
           L"unknown restart mode is rejected");

    const std::array<std::wstring, 4> zero_process{
        L"--apply-update", L"C:\\AirScreenshot.exe", L"0", L"no-restart"};
    expect(airshot::run_update_helper(zero_process) ==
               static_cast<int>(airshot::ExitCode::invalid_arguments),
           L"zero parent process id is rejected");

    const std::array<std::wstring, 4> missing_process{
        L"--apply-update",
        L"C:\\AirScreenshot.exe",
        std::to_wstring(std::numeric_limits<DWORD>::max()),
        L"no-restart"};
    expect(airshot::run_update_helper(missing_process) ==
               static_cast<int>(airshot::ExitCode::operation_failed),
           L"legacy parent disappearance still requires a trusted signed helper and target");

    const std::array<std::wstring, 4> relative_target{
        L"--apply-update", L"AirScreenshot.exe", L"1", L"no-restart"};
    expect(airshot::run_update_helper(relative_target) ==
               static_cast<int>(airshot::ExitCode::invalid_arguments),
           L"relative replacement target is rejected");

    const std::array<std::wstring, 5> invalid_handshake{
        L"--apply-update",
        L"C:\\AirScreenshot.exe",
        L"1",
        L"no-restart",
        L"Global\\untrusted-event"};
    expect(airshot::run_update_helper(invalid_handshake) ==
               static_cast<int>(airshot::ExitCode::invalid_arguments),
           L"an arbitrary helper handshake event is rejected");
}

std::optional<std::wstring> current_user_sid_string() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return std::nullopt;
    }
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<std::byte> storage(bytes);
    if (bytes == 0 ||
        !GetTokenInformation(
            token,
            TokenUser,
            storage.data(),
            bytes,
            &bytes)) {
        CloseHandle(token);
        return std::nullopt;
    }
    CloseHandle(token);

    const auto* user =
        reinterpret_cast<const TOKEN_USER*>(storage.data());
    wchar_t* raw_sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &raw_sid)) {
        return std::nullopt;
    }
    std::wstring result(raw_sid);
    LocalFree(raw_sid);
    return result;
}

bool set_test_dacl(
    const std::filesystem::path& path,
    std::wstring_view user_sid,
    bool protected_dacl,
    bool include_system) {
    const std::wstring sddl =
        std::format(
            L"D:{}(A;;FA;;;{}){}",
            protected_dacl ? L"P" : L"",
            user_sid,
            include_system ? L"(A;;FA;;;SY)" : L"");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
        return false;
    }
    const SECURITY_INFORMATION information =
        DACL_SECURITY_INFORMATION |
        (protected_dacl
             ? PROTECTED_DACL_SECURITY_INFORMATION
             : UNPROTECTED_DACL_SECURITY_INFORMATION);
    const BOOL applied = SetFileSecurityW(
        path.c_str(), information, descriptor);
    LocalFree(descriptor);
    return applied != FALSE;
}

bool set_test_low_mandatory_label(
    const std::filesystem::path& path,
    std::wstring_view label_sid) {
    const std::wstring sddl =
        std::format(L"S:(ML;;NW;;;{})", label_sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &descriptor,
            nullptr)) {
        return false;
    }
    const BOOL applied = SetFileSecurityW(
        path.c_str(),
        LABEL_SECURITY_INFORMATION,
        descriptor);
    LocalFree(descriptor);
    return applied != FALSE;
}

bool query_mandatory_label_rid(
    const std::filesystem::path& path,
    std::optional<DWORD>* result) {
    if (!result) {
        return false;
    }
    result->reset();
    HANDLE file = CreateFileW(
        path.c_str(),
        READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE |
            FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL |
            FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    constexpr SECURITY_INFORMATION information =
        OWNER_SECURITY_INFORMATION |
        GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION |
        LABEL_SECURITY_INFORMATION;
    DWORD bytes = 0;
    GetKernelObjectSecurity(
        file,
        information,
        nullptr,
        0,
        &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        bytes < SECURITY_DESCRIPTOR_MIN_LENGTH) {
        CloseHandle(file);
        return false;
    }
    std::vector<std::byte> storage(bytes);
    const BOOL queried = GetKernelObjectSecurity(
            file,
            information,
            storage.data(),
            bytes,
            &bytes);
    CloseHandle(file);
    if (!queried) {
        return false;
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL label = nullptr;
    if (!GetSecurityDescriptorSacl(
            storage.data(),
            &present,
            &label,
            &defaulted)) {
        return false;
    }
    if (!present || !label) {
        return true;
    }
    for (DWORD index = 0;
         index < label->AceCount;
         ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(label, index, &raw_ace) ||
            !raw_ace) {
            return false;
        }
        const auto* header =
            static_cast<const ACE_HEADER*>(raw_ace);
        if (header->AceType !=
            SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
            continue;
        }
        const auto* ace =
            static_cast<
                const SYSTEM_MANDATORY_LABEL_ACE*>(
                raw_ace);
        PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if (!IsValidSid(sid)) {
            return false;
        }
        const UCHAR sub_authorities =
            *GetSidSubAuthorityCount(sid);
        if (sub_authorities == 0) {
            return false;
        }
        *result = *GetSidSubAuthority(
            sid, sub_authorities - 1);
        return true;
    }
    return true;
}

std::string read_binary_file(
    const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
}

void test_atomic_replacement_preserves_security() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::format(
                          L"airshot-portable-acl-test-{}-{}",
                          GetCurrentProcessId(),
                          GetTickCount64());
    std::filesystem::create_directories(root);
    const auto target = root / L"target.exe";
    const auto replacement = root / L"replacement.tmp";
    const auto backup = root / L"backup.tmp";
    const auto reference = root / L"reference.exe";
    {
        std::ofstream(target, std::ios::binary) << "old-content";
        std::ofstream(replacement, std::ios::binary) << "new-content";
        std::ofstream(reference, std::ios::binary) << "reference";
    }

    const auto user_sid = current_user_sid_string();
    expect(user_sid.has_value(),
           L"the current user SID is available for ACL testing");
    if (user_sid) {
        expect(set_test_dacl(target, *user_sid, false, true) &&
                   set_test_dacl(reference, *user_sid, false, true) &&
                   set_test_dacl(replacement, *user_sid, true, false),
               L"the ACL test fixtures receive distinct DACLs");

        std::wstring error;
        expect(
            airshot::portable_internal::same_file_security(
                target, reference, &error),
            std::format(
                L"target and reference start with matching security: {}",
                error));
        error.clear();
        expect(
            !airshot::portable_internal::same_file_security(
                target, replacement, &error),
            L"replacement starts with different security");

        error.clear();
        const bool replaced =
            airshot::portable_internal::replace_file_preserving_security(
                target, replacement, backup, &error);
        expect(
            replaced,
            std::format(
                L"the production replacement primitive succeeds: {}",
                error));
        if (replaced) {
            expect(read_binary_file(target) == "new-content",
                   L"replacement content becomes the target");
            expect(read_binary_file(backup) == "old-content",
                   L"the flushed rollback backup keeps original content");
            error.clear();
            expect(
                airshot::portable_internal::same_file_security(
                    target, reference, &error),
                std::format(
                    L"target owner/group/DACL/label are preserved: {}",
                    error));
            error.clear();
            expect(
                airshot::portable_internal::same_file_security(
                    backup, reference, &error),
                std::format(
                    L"rollback backup preserves target security: {}",
                    error));
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_named_object_security_owner() {
    std::wstring error;
    expect(
        airshot::portable_internal::
            named_object_security_uses_current_user_owner(
                &error),
        std::format(
            L"named update objects explicitly use the current user as owner: {}",
            error));
}

void test_security_repair_retries_transient_sharing_violation() {
    using namespace std::chrono_literals;
    const auto root = std::filesystem::temp_directory_path() /
                      std::format(
                          L"airshot-portable-security-retry-test-{}-{}",
                          GetCurrentProcessId(),
                          GetTickCount64());
    std::filesystem::create_directories(root);
    const auto reference = root / L"reference.exe";
    const auto destination = root / L"destination.exe";
    {
        std::ofstream(reference, std::ios::binary) << "reference";
        std::ofstream(destination, std::ios::binary) << "destination";
    }

    const auto user_sid = current_user_sid_string();
    expect(user_sid.has_value(),
           L"the current user SID is available for security retry testing");
    if (user_sid) {
        expect(set_test_dacl(reference, *user_sid, false, true) &&
                   set_test_dacl(destination, *user_sid, true, false),
               L"security retry fixtures receive different DACLs");

        const HANDLE blocker = CreateFileW(
            destination.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        expect(blocker != INVALID_HANDLE_VALUE,
               L"security retry fixture blocks write access");
        if (blocker != INVALID_HANDLE_VALUE) {
            std::jthread release_blocker([blocker] {
                std::this_thread::sleep_for(150ms);
                CloseHandle(blocker);
            });
            std::wstring error;
            const bool repaired =
                airshot::portable_internal::apply_file_security_from_reference(
                    reference, destination, &error);
            release_blocker.join();
            expect(
                repaired,
                std::format(
                    L"security repair retries a transient sharing violation: {}",
                    error));
            error.clear();
            expect(
                repaired &&
                    airshot::portable_internal::same_file_security(
                        reference, destination, &error),
                std::format(
                    L"security retry repairs the intended stable file: {}",
                    error));
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_mandatory_label_round_trip() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::format(
                          L"airshot-portable-label-test-{}-{}",
                          GetCurrentProcessId(),
                          GetTickCount64());
    std::filesystem::create_directories(root);
    const auto reference = root / L"reference.exe";
    const auto destination = root / L"destination.exe";
    {
        std::ofstream(reference, std::ios::binary)
            << "reference";
        std::ofstream(destination, std::ios::binary)
            << "destination";
    }

    std::optional<DWORD> reference_label;
    expect(
        query_mandatory_label_rid(
            reference, &reference_label),
        L"the label reference security descriptor is readable");
    const bool reference_is_low =
        reference_label &&
        *reference_label ==
            SECURITY_MANDATORY_LOW_RID;
    const std::wstring_view fixture_sid =
        reference_is_low ? L"UN" : L"LW";
    const DWORD fixture_rid =
        reference_is_low
            ? SECURITY_MANDATORY_UNTRUSTED_RID
            : SECURITY_MANDATORY_LOW_RID;
    expect(
        set_test_low_mandatory_label(
            destination, fixture_sid),
        L"the label fixture receives a different mandatory-integrity label");
    std::optional<DWORD> fixture_label;
    expect(
        query_mandatory_label_rid(
            destination, &fixture_label) &&
            fixture_label &&
            *fixture_label == fixture_rid &&
            (!reference_label ||
             *fixture_label != *reference_label),
        L"the label fixture exposes the selected mandatory-label RID");
    std::wstring error;
    expect(
        !airshot::portable_internal::same_file_security(
            reference, destination, &error),
        L"the explicit low label differs from an absent label");
    error.clear();
    expect(
        airshot::portable_internal::
            apply_file_security_from_reference(
                reference, destination, &error),
        std::format(
            L"a mandatory label is replayed exactly: {}",
            error));
    std::optional<DWORD> replayed_label;
    expect(
        query_mandatory_label_rid(
            destination, &replayed_label) &&
            replayed_label == reference_label,
        L"security replay restores the reference mandatory-label RID");
    error.clear();
    expect(
        airshot::portable_internal::same_file_security(
            reference, destination, &error),
        std::format(
            L"the real-file label round trip preserves the descriptor: {}",
            error));

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_security_descriptor_sacl_comparison() {
    std::wstring error;
    expect(
        airshot::portable_internal::
            security_descriptor_comparison_checks_sacl_state(
                &error),
        std::format(
            L"security comparison normalizes null labels and covers SACL protection: {}",
            error));
}

void test_rollback_reconciles_post_commit_failure() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::format(
                          L"airshot-portable-rollback-test-{}-{}",
                          GetCurrentProcessId(),
                          GetTickCount64());
    std::filesystem::create_directories(root);

    const auto run_case =
        [&root](std::wstring_view name, bool target_exists) {
            const auto target =
                root / std::format(L"{}-target.exe", name);
            const auto backup =
                root / std::format(L"{}-backup.tmp", name);
            if (target_exists) {
                std::ofstream(target, std::ios::binary)
                    << "new-content";
            }
            std::ofstream(backup, std::ios::binary)
                << "old-content";

            std::wstring error;
            expect(
                airshot::portable_internal::
                    rollback_post_commit_failure_is_reconciled(
                        target, backup, &error),
                std::format(
                    L"post-commit rollback is reconciled for {} target: {}",
                    target_exists ? L"an existing" : L"a missing",
                    error));
            expect(
                read_binary_file(target) == "old-content",
                L"the reconciled rollback restores old content");
            expect(
                !std::filesystem::exists(backup),
                L"a consumed rollback backup is not reported as retained");
        };

    run_case(L"replace", true);
    run_case(L"move", false);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_update_cancellation() {
    std::stop_source source;
    source.request_stop();
    std::wstring message = L"stale";
    expect(
        airshot::stage_latest_update(&message, source.get_token()) ==
                airshot::UpdateStageResult::failed &&
            message.find(L"取消") != std::wstring::npos,
        L"a pre-cancelled update check exits before network or filesystem work");
}

void test_no_pending_update_is_silent() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::format(
                          L"airshot-portable-empty-test-{}-{}",
                          GetCurrentProcessId(),
                          GetTickCount64());
    const DWORD old_size =
        GetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr, 0);
    std::wstring old_value(old_size, L'\0');
    if (old_size > 0) {
        const DWORD copied = GetEnvironmentVariableW(
            L"AIRSHOT_DATA_DIR", old_value.data(), old_size);
        old_value.resize(copied);
    }
    SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", root.c_str());

    std::wstring error = L"stale";
    expect(!airshot::launch_pending_update(false, &error) && error.empty(),
           L"a normal startup without an updates directory is silent");
    std::filesystem::create_directories(root / L"updates");
    error = L"stale";
    expect(!airshot::launch_pending_update(false, &error) && error.empty(),
           L"a normal startup without pending.json is silent");

    if (old_size > 0) {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", old_value.c_str());
    } else {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_reparse_update_directory_is_rejected() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::format(
                          L"airshot-portable-reparse-test-{}-{}",
                          GetCurrentProcessId(),
                          GetTickCount64());
    const auto outside = root.parent_path() /
                         std::format(
                             L"airshot-portable-reparse-target-{}-{}",
                             GetCurrentProcessId(),
                             GetTickCount64());
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(outside);

    const DWORD old_size =
        GetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr, 0);
    std::wstring old_value(old_size, L'\0');
    if (old_size > 0) {
        const DWORD copied = GetEnvironmentVariableW(
            L"AIRSHOT_DATA_DIR", old_value.data(), old_size);
        old_value.resize(copied);
    }
    SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", root.c_str());

    const auto updates_link = root / L"updates";
    constexpr DWORD allow_unprivileged_create = 0x2;
    const BOOL linked = CreateSymbolicLinkW(
        updates_link.c_str(),
        outside.c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | allow_unprivileged_create);
    if (linked) {
        std::wstring error;
        expect(!airshot::launch_pending_update(false, &error) &&
                   !error.empty(),
               L"a reparse-point updates directory is rejected");
        RemoveDirectoryW(updates_link.c_str());
    } else {
        const DWORD link_error = GetLastError();
        expect(link_error == ERROR_PRIVILEGE_NOT_HELD ||
                   link_error == ERROR_NOT_SUPPORTED,
               L"the reparse-point test is skipped only when symlinks are unavailable");
    }

    if (old_size > 0) {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", old_value.c_str());
    } else {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::remove_all(outside, ignored);
}

void test_live_update_endpoint() {
    const auto root = std::filesystem::temp_directory_path() /
                      std::format(
                          L"airshot-portable-network-test-{}-{}",
                          GetCurrentProcessId(),
                          GetTickCount64());
    const DWORD old_size =
        GetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr, 0);
    std::wstring old_value(old_size, L'\0');
    if (old_size > 0) {
        const DWORD copied = GetEnvironmentVariableW(
            L"AIRSHOT_DATA_DIR", old_value.data(), old_size);
        old_value.resize(copied);
    }
    SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", root.c_str());

    std::wstring message;
    const auto result = airshot::stage_latest_update(&message);
    expect(result != airshot::UpdateStageResult::failed,
           std::format(L"live update endpoint succeeds: {}", message));

    if (old_size > 0) {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", old_value.c_str());
    } else {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_in_flight_update_cancellation() {
    using namespace std::chrono_literals;
    const auto root = std::filesystem::temp_directory_path() /
                      std::format(
                          L"airshot-portable-cancel-test-{}-{}",
                          GetCurrentProcessId(),
                          GetTickCount64());
    const DWORD old_size =
        GetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr, 0);
    std::wstring old_value(old_size, L'\0');
    if (old_size > 0) {
        const DWORD copied = GetEnvironmentVariableW(
            L"AIRSHOT_DATA_DIR", old_value.data(), old_size);
        old_value.resize(copied);
    }
    SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", root.c_str());

    std::latch entered(1);
    airshot::UpdateStageResult result = airshot::UpdateStageResult::up_to_date;
    std::wstring message;
    const auto started = std::chrono::steady_clock::now();
    std::jthread worker([&](std::stop_token stop_token) {
        entered.count_down();
        result = airshot::stage_latest_update(&message, stop_token);
    });
    entered.wait();
    std::this_thread::sleep_for(25ms);
    worker.request_stop();
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    expect(result == airshot::UpdateStageResult::failed &&
               message.find(L"取消") != std::wstring::npos,
           L"an in-flight update observes cancellation");
    expect(elapsed < 8s,
           L"asynchronous WinHTTP cancellation returns without waiting indefinitely for handle closure");

    if (old_size > 0) {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", old_value.c_str());
    } else {
        SetEnvironmentVariableW(L"AIRSHOT_DATA_DIR", nullptr);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int wmain(int argument_count, wchar_t** arguments) {
    airshot::ScopedWinrtApartment apartment;
    expect(apartment.available(), L"Windows Runtime is available");
    test_versions();
    test_manifests();
    test_pending_update_sources();
    test_mark_pending_update_manual_on_disk();
    test_hashing();
    test_helper_boundary();
    test_atomic_replacement_preserves_security();
    test_named_object_security_owner();
    test_security_repair_retries_transient_sharing_violation();
    test_mandatory_label_round_trip();
    test_security_descriptor_sacl_comparison();
    test_rollback_reconciles_post_commit_failure();
    test_update_cancellation();
    test_no_pending_update_is_silent();
    test_reparse_update_directory_is_rejected();
    if (argument_count == 2 &&
        std::wstring_view(arguments[1]) == L"--network") {
        test_in_flight_update_cancellation();
        test_live_update_endpoint();
    }
    if (failures == 0) {
        std::wcout << L"portable updater tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
