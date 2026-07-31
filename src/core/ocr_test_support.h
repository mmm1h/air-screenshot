#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "airshot/ocr.h"

namespace airshot::ocr_test_support {

struct WarmWorkerResponseResult {
    bool ok{};
    OcrOutput output;
    std::wstring worker_error;
};

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

[[nodiscard]] Bitmap resize_bitmap_high_quality(
    const Bitmap& source,
    int target_width,
    int target_height);

[[nodiscard]] double select_preprocess_scale(
    int width,
    int height) noexcept;

[[nodiscard]] int select_thread_count(
    std::uint64_t pixels,
    unsigned int logical_processors,
    bool accurate_profile) noexcept;

[[nodiscard]] std::optional<WarmWorkerResponseResult>
parse_warm_worker_response(
    std::string_view response_utf8,
    std::uint64_t expected_request_id,
    std::wstring_view expected_profile,
    const std::filesystem::path& expected_root,
    std::uint64_t expected_sequence,
    const OcrProtocolExpectations& expected_image,
    std::wstring* protocol_error = nullptr);

[[nodiscard]] bool warm_worker_key_matches(
    bool process_healthy,
    const std::filesystem::path& current_root,
    std::uint64_t current_sequence,
    std::wstring_view current_profile,
    int current_ort_threads,
    const std::filesystem::path& requested_root,
    std::uint64_t requested_sequence,
    std::wstring_view requested_profile,
    int requested_ort_threads);

[[nodiscard]] bool dependency_hash_cache_key_matches(
    bool same_file_object,
    const std::filesystem::path& cached_root,
    std::uint64_t cached_sequence,
    std::wstring_view cached_relative_path,
    std::wstring_view cached_sha256,
    std::uint64_t cached_size,
    const std::filesystem::path& requested_root,
    std::uint64_t requested_sequence,
    std::wstring_view requested_relative_path,
    std::wstring_view requested_sha256,
    std::uint64_t requested_size);

[[nodiscard]] std::optional<std::wstring> sha256_file(
    const std::filesystem::path& path,
    std::stop_token stop_token,
    std::wstring* error = nullptr);

[[nodiscard]] bool warm_failure_allows_fallback(
    bool cancelled,
    bool stop_requested) noexcept;

[[nodiscard]] std::optional<std::uint32_t> decode_warm_frame_size(
    std::span<const char> header,
    std::size_t maximum) noexcept;

[[nodiscard]] HANDLE lock_path(
    const std::filesystem::path& path,
    bool directory,
    std::wstring* error = nullptr);

[[nodiscard]] OcrDependencyStatus classify_preparation_failure(
    std::wstring error,
    bool cancelled = false);

}  // namespace airshot::ocr_test_support
