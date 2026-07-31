#pragma once

#include "airshot/common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace airshot::capture_uia {

// UI Automation providers live in other processes and are not trusted to be
// fast or well behaved. Keep every request and result bounded so the HWND path
// remains the authoritative, immediate fallback.
inline constexpr std::size_t kMaxCandidateCount = 16;
inline constexpr std::size_t kMaxProviderDepth = 24;
inline constexpr std::size_t kMaxVisitedElements = 96;
inline constexpr int kMinimumCandidateExtent = 7;
inline constexpr int kMaximumCandidateExtent = 65'535;
inline constexpr std::uint64_t kMaximumCandidateArea = 1'073'741'824ULL;
inline constexpr std::uint64_t kProviderBudgetMilliseconds = 450;
inline constexpr std::uint64_t kCacheLifetimeMilliseconds = 800;
inline constexpr int kCacheReuseRadius = 4;
inline constexpr std::uint64_t kRequestCoalesceMilliseconds = 120;
inline constexpr int kRequestCoalesceRadius = 2;

struct Request {
    std::uint64_t session{};
    std::uint64_t sequence{};
    POINT point{};
    HWND root{};
    RectI root_bounds;
    std::uint64_t submitted_at{};
};

// This policy is deliberately independent from COM and threading. The worker
// and its tests use the same state transitions for latest-request coalescing,
// session invalidation, stale-result rejection, and provider time budgets.
struct MailboxPolicy {
    std::uint64_t session{1};
    std::uint64_t next_sequence{};
    std::uint64_t newest_sequence{};
    POINT newest_point{};
    HWND newest_root{};
    std::uint64_t newest_submitted_at{};
    bool has_newest{};
    bool has_pending{};
    Request pending;
};

[[nodiscard]] std::uint64_t begin_session(MailboxPolicy& policy) noexcept;
[[nodiscard]] bool queue_latest(
    MailboxPolicy& policy,
    POINT point,
    HWND root,
    const RectI& root_bounds,
    std::uint64_t now) noexcept;
[[nodiscard]] std::optional<Request> take_latest(
    MailboxPolicy& policy) noexcept;
[[nodiscard]] bool may_publish(
    const MailboxPolicy& policy,
    const Request& request,
    std::uint64_t now) noexcept;

[[nodiscard]] std::optional<RectI> sanitize_provider_rect(
    double left,
    double top,
    double width,
    double height,
    const RectI& root_bounds,
    const RectI& desktop_bounds,
    POINT point) noexcept;

struct CandidateChain {
    HWND root{};
    POINT point{};
    std::uint64_t session{};
    std::uint64_t sequence{};
    std::uint64_t completed_at{};
    std::array<RectI, kMaxCandidateCount> candidates{};
    std::size_t count{};
};

[[nodiscard]] bool may_reuse_cache(
    const CandidateChain& cache,
    POINT point,
    HWND root,
    std::uint64_t session,
    std::uint64_t now) noexcept;

// Starts a new logical capture session and invalidates cached UIA candidates.
// The process owns at most one worker for its entire lifetime.
void begin_candidate_session() noexcept;

// Never calls COM or a UIA provider. It only reads a fresh cached result,
// coalesces the newest point request, and wakes the single background worker.
[[nodiscard]] CandidateChain cached_chain_and_request(
    POINT point,
    HWND root,
    const RectI& root_bounds) noexcept;

}  // namespace airshot::capture_uia
