#pragma once

#include "airshot/capture.h"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace airshot::overlay_detail {

enum class SnapshotRefreshStatus {
    compatible,
    topology_changed,
    capture_failed,
};

[[nodiscard]] inline bool same_monitor_identity(
    const MonitorSnapshot& first,
    const MonitorSnapshot& second) noexcept {
    return first.bounds.left == second.bounds.left &&
           first.bounds.top == second.bounds.top &&
           first.bounds.right == second.bounds.right &&
           first.bounds.bottom == second.bounds.bottom &&
           first.primary == second.primary &&
           first.device_name == second.device_name;
}

// Validates a complete replacement before any live monitor bitmap is moved.
// This preserves the old screenshot if a display is connected, disconnected,
// rotated, resized, or if any individual capture fails during F5 refresh.
[[nodiscard]] inline SnapshotRefreshStatus validate_snapshot_refresh(
    std::span<const MonitorSnapshot> current,
    std::span<const MonitorSnapshot> refreshed,
    std::vector<std::size_t>* refreshed_indices = nullptr) {
    if (current.size() != refreshed.size() || current.empty()) {
        return SnapshotRefreshStatus::topology_changed;
    }

    std::vector<std::size_t> indices(current.size(), refreshed.size());
    std::vector<bool> used(refreshed.size(), false);
    for (std::size_t current_index = 0;
         current_index < current.size();
         ++current_index) {
        for (std::size_t refreshed_index = 0;
             refreshed_index < refreshed.size();
             ++refreshed_index) {
            if (!used[refreshed_index] &&
                same_monitor_identity(
                    current[current_index],
                    refreshed[refreshed_index])) {
                indices[current_index] = refreshed_index;
                used[refreshed_index] = true;
                break;
            }
        }
        if (indices[current_index] == refreshed.size()) {
            return SnapshotRefreshStatus::topology_changed;
        }
        const MonitorSnapshot& replacement =
            refreshed[indices[current_index]];
        if (!replacement.bitmap.valid() ||
            replacement.bitmap.width != replacement.bounds.width() ||
            replacement.bitmap.height != replacement.bounds.height()) {
            return SnapshotRefreshStatus::capture_failed;
        }
    }

    if (refreshed_indices) {
        *refreshed_indices = std::move(indices);
    }
    return SnapshotRefreshStatus::compatible;
}

[[nodiscard]] inline bool apply_snapshot_refresh(
    std::span<MonitorSnapshot> current,
    std::vector<MonitorSnapshot>& refreshed) {
    std::vector<std::size_t> indices;
    if (validate_snapshot_refresh(current, refreshed, &indices) !=
        SnapshotRefreshStatus::compatible) {
        return false;
    }
    for (std::size_t index = 0; index < current.size(); ++index) {
        MonitorSnapshot& destination = current[index];
        MonitorSnapshot& source = refreshed[indices[index]];
        destination.handle = source.handle;
        destination.bitmap = std::move(source.bitmap);
    }
    return true;
}

}  // namespace airshot::overlay_detail
