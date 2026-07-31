#pragma once

#include "airshot/bitmap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace airshot {

struct AppConfig;

inline constexpr std::uint64_t kOcrProtocolSchemaVersion = 1;

struct OcrPoint {
    double x{};
    double y{};
};

struct OcrBlock {
    std::array<OcrPoint, 4> quad{};
    std::wstring text;
    double score{};
};

struct OcrTimings {
    double decode_ms{};
    double model_init_ms{};
    double inference_ms{};
    double merge_ms{};
    double total_ms{};
};

struct OcrPreprocessInfo {
    int source_width{};
    int source_height{};
    int input_width{};
    int input_height{};
    double scale_x{1.0};
    double scale_y{1.0};
    std::wstring resample;
    bool tiled{};
    int tile_count{1};
    int tile_size{};
    int tile_overlap{};
};

struct OcrOutput {
    bool ok{};
    std::wstring text;
    std::wstring error;
    std::vector<OcrBlock> blocks;
    std::wstring profile;
    OcrPreprocessInfo preprocess;
    OcrTimings timings;
};

struct OcrProtocolExpectations {
    std::wstring_view profile;
    int source_width{};
    int source_height{};
    int input_width{};
    int input_height{};
    double scale_x{};
    double scale_y{};
    std::wstring_view resample;
};

struct OcrDependencyFile {
    std::wstring path;
    std::wstring url;
    std::wstring sha256;
    std::uint64_t size{};
};

struct OcrDependencyManifest {
    std::wstring package_id;
    std::uint64_t sequence{};
    std::uint64_t issued_at{};
    std::uint64_t expires_at{};
    std::vector<OcrDependencyFile> files;
};

struct OcrManifestSignature {
    std::wstring key_id;
    std::array<std::uint8_t, 64> value{};
};

// Stable product-facing states for dependency preparation. UI code should use
// these values instead of parsing localized status/error strings.
enum class OcrDependencyState : std::uint8_t {
    checking,
    ready,
    download_required,
    offline,
    downloading_manifest,
    verifying_manifest,
    downloading_files,
    installing,
    verifying_installation,
    cancelled,
    retryable_error,
    repair_required,
    unavailable,
};

enum class OcrRecoveryAction : std::uint8_t {
    none,
    download,
    retry,
    go_online,
    repair,
    check_system_time,
    configure_build,
    contact_support,
};

struct OcrDependencyStatus {
    bool ready{};
    bool can_download{};
    std::wstring message;
    OcrDependencyState state{OcrDependencyState::unavailable};
    OcrRecoveryAction recommended_action{OcrRecoveryAction::none};
    bool usable_offline{};
    bool requires_network{};
    bool retryable{};
    bool security_blocked{};
    std::wstring detail;
};

struct OcrPreparationProgress {
    OcrDependencyState state{OcrDependencyState::checking};
    // Monotonic for one prepare call. Payload byte/file counters become known
    // after the signed manifest has been accepted.
    int percent{};
    std::size_t completed_files{};
    std::size_t total_files{};
    std::uint64_t downloaded_bytes{};
    std::uint64_t total_bytes{};
    bool cancellable{true};
    std::wstring message;
};

using OcrPreparationProgressCallback =
    std::function<void(const OcrPreparationProgress&)>;

struct OcrPreparationOptions {
    std::wstring engine;
    std::wstring manifest_url;
    // Set false when the caller knows it is offline. An already verified local
    // installation remains usable; missing dependencies fail without network
    // I/O and return go_online as the recommended action.
    bool allow_network{true};
};

struct OcrPreparationResult {
    OcrDependencyStatus status;
    bool used_existing{};
    bool cancelled{};
};

struct OcrRepairResult {
    // ok means the repair operation itself completed without a filesystem or
    // cancellation error. The resulting availability is always represented by
    // status because a packaged read-only component may still need support.
    bool ok{};
    bool changed{};
    std::filesystem::path preserved_path;
    OcrDependencyStatus status;
    std::wstring error;
};

class OcrDependencyLease {
public:
    OcrDependencyLease() = default;
    ~OcrDependencyLease();
    OcrDependencyLease(const OcrDependencyLease&) = delete;
    OcrDependencyLease& operator=(const OcrDependencyLease&) = delete;
    OcrDependencyLease(OcrDependencyLease&& other) noexcept;
    OcrDependencyLease& operator=(OcrDependencyLease&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::span<const HANDLE> handles() const noexcept;

private:
    friend std::optional<OcrDependencyLease> acquire_ocr_dependency_lease(
        const std::filesystem::path& root,
        bool verify_hashes,
        bool update_high_watermark,
        std::wstring* error,
        std::stop_token stop_token);

    OcrDependencyLease(
        std::vector<HANDLE> handles,
        std::uint64_t sequence,
        std::filesystem::path root) noexcept;
    void reset() noexcept;

    std::vector<HANDLE> handles_;
    std::uint64_t sequence_{};
    std::filesystem::path root_;
};

[[nodiscard]] OcrOutput recognize_text(
    const Bitmap& bitmap,
    const AppConfig& config,
    std::stop_token stop_token = {});
[[nodiscard]] std::optional<OcrOutput> parse_ocr_runner_protocol(
    std::string_view json_utf8,
    const OcrProtocolExpectations& expected,
    std::wstring* error = nullptr);
[[nodiscard]] std::wstring join_ocr_lines(std::span<const std::wstring> lines);
[[nodiscard]] std::filesystem::path rapid_ocr_dependency_directory();
[[nodiscard]] std::uint64_t ocr_minimum_sequence() noexcept;
[[nodiscard]] std::optional<OcrDependencyLease> acquire_ocr_dependency_lease(
    const std::filesystem::path& root,
    bool verify_hashes,
    bool update_high_watermark,
    std::wstring* error = nullptr,
    std::stop_token stop_token = {});
[[nodiscard]] std::optional<OcrDependencyManifest> parse_ocr_dependency_manifest(std::wstring_view json);
[[nodiscard]] std::optional<OcrManifestSignature> parse_ocr_manifest_signature(std::wstring_view json);
[[nodiscard]] bool verify_ocr_manifest_signature(
    std::span<const std::byte> manifest_utf8,
    std::span<const std::uint8_t> signature,
    std::span<const std::uint8_t> public_key_xy,
    std::wstring* error = nullptr);
// Lightweight display status. Call prepare_ocr_dependencies before recognition
// when a security-authoritative, full-hash readiness decision is required.
[[nodiscard]] OcrDependencyStatus ocr_dependency_status(std::wstring_view engine);
// Idempotent first-use preparation entry point. It reuses a verified local
// package when available and otherwise performs the existing signed-manifest,
// anti-rollback, per-file SHA256, staged-install and post-install verification
// pipeline. Retry by calling this function again with the same options.
[[nodiscard]] OcrPreparationResult prepare_ocr_dependencies(
    const OcrPreparationOptions& options,
    const OcrPreparationProgressCallback& progress_callback = {},
    std::stop_token stop_token = {});
[[nodiscard]] OcrPreparationResult prepare_ocr_dependencies(
    const AppConfig& config,
    bool allow_network,
    const OcrPreparationProgressCallback& progress_callback = {},
    std::stop_token stop_token = {});
// Explicit repair action for a failed local installation. Invalid user-local
// data is moved into a private quarantine directory and preserved for
// diagnostics; it is never silently deleted and packaged read-only data is
// never modified.
[[nodiscard]] OcrRepairResult repair_ocr_dependencies(
    std::wstring_view engine,
    std::stop_token stop_token = {});
bool download_ocr_dependencies(
    std::wstring_view manifest_url,
    const std::function<void(int)>& progress_callback,
    std::wstring* error = nullptr,
    std::stop_token stop_token = {});

int run_ocr_manifest_verifier(std::span<const std::wstring> arguments);
int run_ocr_cli(std::span<const std::wstring> arguments);
int run_ocr_warm_smoke(std::span<const std::wstring> arguments);

}  // namespace airshot
