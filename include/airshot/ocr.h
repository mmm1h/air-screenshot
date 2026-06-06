#pragma once

#include "airshot/bitmap.h"

#include <functional>

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
    std::vector<OcrDependencyFile> files;
};

struct OcrDependencyStatus {
    bool ready{};
    bool can_download{};
    std::wstring message;
};

[[nodiscard]] OcrOutput recognize_text(const Bitmap& bitmap, const AppConfig& config);
[[nodiscard]] std::wstring join_ocr_lines(std::span<const std::wstring> lines);
[[nodiscard]] std::filesystem::path rapid_ocr_dependency_directory();
[[nodiscard]] std::optional<OcrDependencyManifest> parse_ocr_dependency_manifest(std::wstring_view json);
[[nodiscard]] OcrDependencyStatus ocr_dependency_status(std::wstring_view engine);
bool download_ocr_dependencies(
    std::wstring_view manifest_url,
    const std::function<void(int)>& progress_callback,
    std::wstring* error = nullptr);

}  // namespace airshot
