#pragma once

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace airshot {

inline constexpr wchar_t kAppName[] = L"Air Screenshot";
inline constexpr wchar_t kAppWindowClass[] = L"AirScreenshot.HostWindow";
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\LOCAL\\AirScreenshot.v1";
inline constexpr wchar_t kHostMutexName[] = L"Local\\AirScreenshot.Host.v1";
inline constexpr wchar_t kLatestJsonUrl[] = L"https://mmm1h.github.io/air-screenshot/latest.json";
inline constexpr wchar_t kReleaseSignerSha256[] =
    L"CB864DBFC63D94625D2173DD9B5C0426A88A415A29F054D2936165FA793D7E1E";

enum class ExitCode : int {
    success = 0,
    unknown_error = 1,
    invalid_arguments = 2,
    user_cancelled = 3,
    module_unavailable = 4,
    operation_failed = 5,
    ipc_failed = 6,
};

class ScopedWinrtApartment {
public:
    explicit ScopedWinrtApartment(bool single_threaded = false) noexcept;
    ~ScopedWinrtApartment();
    ScopedWinrtApartment(const ScopedWinrtApartment&) = delete;
    ScopedWinrtApartment& operator=(const ScopedWinrtApartment&) = delete;

    [[nodiscard]] bool available() const noexcept;

private:
    HRESULT result_{};
};

struct RectI {
    int left{};
    int top{};
    int right{};
    int bottom{};

    [[nodiscard]] int width() const noexcept { return right - left; }
    [[nodiscard]] int height() const noexcept { return bottom - top; }
    [[nodiscard]] bool empty() const noexcept { return width() <= 0 || height() <= 0; }
    [[nodiscard]] bool contains(POINT point) const noexcept {
        return point.x >= left && point.x < right && point.y >= top && point.y < bottom;
    }

    [[nodiscard]] RectI normalized() const noexcept {
        return {
            std::min(left, right),
            std::min(top, bottom),
            std::max(left, right),
            std::max(top, bottom),
        };
    }

    [[nodiscard]] RECT native() const noexcept { return RECT{left, top, right, bottom}; }

    static RectI from_native(const RECT& value) noexcept {
        return {value.left, value.top, value.right, value.bottom};
    }
};

[[nodiscard]] inline std::optional<RectI> intersect(const RectI& first, const RectI& second) {
    RectI value{
        std::max(first.left, second.left),
        std::max(first.top, second.top),
        std::min(first.right, second.right),
        std::min(first.bottom, second.bottom),
    };
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string to_utf8(std::wstring_view value);
[[nodiscard]] std::wstring from_utf8(std::string_view value);
[[nodiscard]] std::wstring windows_error_message(DWORD error);
[[nodiscard]] std::wstring timestamp_for_file();

}  // namespace airshot
