#pragma once

#include <cstddef>
#include <cstdint>

namespace airshot::output_test {

// One-based index of the required clipboard format whose staging should fail.
// Zero disables fault injection. This hook is intentionally limited to the
// pre-commit staging phase so tests can prove that the existing clipboard is
// never touched by a partial multi-format publication.
void set_required_clipboard_format_failure_for_testing(
    std::size_t one_based_index) noexcept;

// Makes the next publication fail its first OleFlushClipboard call. The
// rollback flush remains real so integration tests can prove preservation of
// the prior clipboard object.
void set_clipboard_flush_failure_for_testing(bool enabled) noexcept;

// Test-only timing controls for the detached-worker path. A zero wait timeout
// restores the production default; worker delay zero disables the delay.
void set_clipboard_worker_delay_for_testing(
    std::uint32_t milliseconds) noexcept;
void set_clipboard_wait_timeout_for_testing(
    std::uint32_t milliseconds) noexcept;
void set_clipboard_pre_flush_delay_for_testing(
    std::uint32_t milliseconds) noexcept;
// Closes or opens a test-only gate immediately after OleSetClipboard succeeds
// and before the worker verifies ownership and flushes. Tests use this to let
// a concurrent publisher finish deterministically instead of relying on a
// scheduler-sensitive fixed delay.
void set_clipboard_forward_set_gate_for_testing(bool blocked) noexcept;
void set_clipboard_snapshot_failure_for_testing(bool enabled) noexcept;
[[nodiscard]] bool clipboard_commit_in_flight_for_testing() noexcept;
[[nodiscard]] bool clipboard_forward_set_pending_for_testing() noexcept;

}  // namespace airshot::output_test
