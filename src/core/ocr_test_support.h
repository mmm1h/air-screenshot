#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace airshot::ocr_test_support {

[[nodiscard]] std::optional<std::uint64_t>
parse_sequence(std::string_view value);

[[nodiscard]] bool sequence_is_allowed(
    std::uint64_t sequence,
    std::uint64_t persisted) noexcept;

[[nodiscard]] std::optional<std::uint64_t>
read_sequence_file(
    const std::filesystem::path& path,
    bool allow_missing,
    std::wstring* error = nullptr);

bool update_sequence_file(
    const std::filesystem::path& path,
    std::uint64_t sequence,
    std::wstring* error = nullptr);

[[nodiscard]] HANDLE lock_path(
    const std::filesystem::path& path,
    bool directory,
    std::wstring* error = nullptr);

}  // namespace airshot::ocr_test_support
