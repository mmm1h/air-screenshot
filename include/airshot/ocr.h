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

struct OcrOutput {
    bool ok{};
    std::wstring text;
    std::wstring error;
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

struct OcrDependencyStatus {
    bool ready{};
    bool can_download{};
    std::wstring message;
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
        std::wstring* error);

    OcrDependencyLease(
        std::vector<HANDLE> handles,
        std::uint64_t sequence,
        std::filesystem::path root) noexcept;
    void reset() noexcept;

    std::vector<HANDLE> handles_;
    std::uint64_t sequence_{};
    std::filesystem::path root_;
};

[[nodiscard]] OcrOutput recognize_text(const Bitmap& bitmap, const AppConfig& config);
[[nodiscard]] std::wstring join_ocr_lines(std::span<const std::wstring> lines);
[[nodiscard]] std::filesystem::path rapid_ocr_dependency_directory();
[[nodiscard]] std::uint64_t ocr_minimum_sequence() noexcept;
[[nodiscard]] std::optional<OcrDependencyLease> acquire_ocr_dependency_lease(
    const std::filesystem::path& root,
    bool verify_hashes,
    bool update_high_watermark,
    std::wstring* error = nullptr);
[[nodiscard]] std::optional<OcrDependencyManifest> parse_ocr_dependency_manifest(std::wstring_view json);
[[nodiscard]] std::optional<OcrManifestSignature> parse_ocr_manifest_signature(std::wstring_view json);
[[nodiscard]] bool verify_ocr_manifest_signature(
    std::span<const std::byte> manifest_utf8,
    std::span<const std::uint8_t> signature,
    std::span<const std::uint8_t> public_key_xy,
    std::wstring* error = nullptr);
[[nodiscard]] OcrDependencyStatus ocr_dependency_status(std::wstring_view engine);
bool download_ocr_dependencies(
    std::wstring_view manifest_url,
    const std::function<void(int)>& progress_callback,
    std::wstring* error = nullptr,
    std::stop_token stop_token = {});

int run_ocr_manifest_verifier(std::span<const std::wstring> arguments);
int run_ocr_cli(std::span<const std::wstring> arguments);

}  // namespace airshot
