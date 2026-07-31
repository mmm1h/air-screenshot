#pragma once

#include "airshot/portable.h"

namespace airshot::portable_internal {

[[nodiscard]] std::optional<UpdateRequestSource>
pending_update_source_from_json(std::wstring_view json);

[[nodiscard]] std::wstring pending_update_to_json_for_testing(
    const UpdateManifest& manifest,
    UpdateRequestSource source);

[[nodiscard]] bool pending_update_source_allowed(
    UpdateRequestSource source,
    bool allow_automatic_pending) noexcept;

bool named_object_security_uses_current_user_owner(
    std::wstring* error = nullptr);

bool security_descriptor_comparison_checks_sacl_state(
    std::wstring* error = nullptr);

bool apply_file_security_from_reference(
    const std::filesystem::path& reference,
    const std::filesystem::path& destination,
    std::wstring* error = nullptr);

bool rollback_post_commit_failure_is_reconciled(
    const std::filesystem::path& target,
    const std::filesystem::path& backup,
    std::wstring* error = nullptr);

bool replace_file_preserving_security(
    const std::filesystem::path& target,
    const std::filesystem::path& replacement,
    const std::filesystem::path& backup,
    std::wstring* error = nullptr);

bool same_file_security(
    const std::filesystem::path& first,
    const std::filesystem::path& second,
    std::wstring* error = nullptr);

}  // namespace airshot::portable_internal
