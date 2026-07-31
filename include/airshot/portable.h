#pragma once

#include "airshot/common.h"

#include <stop_token>

namespace airshot {

inline constexpr std::uint64_t kMaxPortableUpdateBytes = 50ULL * 1024ULL * 1024ULL;

struct UpdateManifest {
    std::wstring version;
    std::wstring url;
    std::wstring sha256;
    std::uint64_t size{};
};

enum class UpdateStageResult {
    up_to_date,
    staged,
    failed,
};

[[nodiscard]] std::filesystem::path portable_executable_path();
[[nodiscard]] std::wstring portable_startup_command(const std::filesystem::path& executable);
bool sync_portable_startup(bool enabled, std::wstring* error = nullptr);

[[nodiscard]] bool version_is_newer(std::wstring_view current, std::wstring_view candidate);
[[nodiscard]] std::optional<UpdateManifest> parse_update_manifest(std::wstring_view json);
[[nodiscard]] std::wstring update_manifest_to_json(const UpdateManifest& manifest);
[[nodiscard]] std::wstring sha256_file(const std::filesystem::path& path, std::wstring* error = nullptr);
[[nodiscard]] bool verify_portable_executable(
    const std::filesystem::path& path, const UpdateManifest& manifest, std::wstring* error = nullptr);
[[nodiscard]] bool update_target_is_replaceable(
    std::wstring* error = nullptr);

UpdateStageResult stage_latest_update(std::wstring* message = nullptr);
UpdateStageResult stage_latest_update(
    std::wstring* message,
    std::stop_token stop_token);
[[nodiscard]] bool pending_update_available();
bool launch_pending_update(bool restart_after_update, std::wstring* error = nullptr);
int run_update_helper(std::span<const std::wstring> arguments);
void cleanup_stale_updates();

}  // namespace airshot
