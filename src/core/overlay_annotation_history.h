#pragma once

#include "overlay_types.h"

#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

namespace airshot::overlay_detail {

// Stores bounded snapshots so every annotation mutation (including erasing,
// moving, styling, and watermark replacement) can participate in undo/redo.
// Screenshot annotations are normally small, but the byte budget prevents a
// long freehand session from retaining unbounded copies of point data.
class AnnotationHistory {
public:
    static constexpr std::size_t kMaximumEntries = 64;
    static constexpr std::size_t kMaximumBytesPerStack = 16U * 1024U * 1024U;

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }

    void record(std::vector<Annotation> before) {
        push(undo_, undo_bytes_, std::move(before));
        clear_stack(redo_, redo_bytes_);
    }

    [[nodiscard]] bool undo(std::vector<Annotation>& current) {
        if (undo_.empty()) {
            return false;
        }
        std::vector<Annotation> next = std::move(undo_.back().annotations);
        undo_bytes_ -= undo_.back().bytes;
        undo_.pop_back();
        push(redo_, redo_bytes_, std::move(current));
        current = std::move(next);
        return true;
    }

    [[nodiscard]] bool redo(std::vector<Annotation>& current) {
        if (redo_.empty()) {
            return false;
        }
        std::vector<Annotation> next = std::move(redo_.back().annotations);
        redo_bytes_ -= redo_.back().bytes;
        redo_.pop_back();
        push(undo_, undo_bytes_, std::move(current));
        current = std::move(next);
        return true;
    }

    void clear_redo() noexcept { clear_stack(redo_, redo_bytes_); }

private:
    struct Entry {
        std::vector<Annotation> annotations;
        std::size_t bytes{};
    };

    static std::size_t estimate_bytes(const std::vector<Annotation>& annotations) noexcept {
        std::size_t bytes = annotations.capacity() * sizeof(Annotation);
        for (const auto& annotation : annotations) {
            bytes += annotation.points.capacity() * sizeof(POINT);
            bytes += annotation.text.capacity() * sizeof(wchar_t);
        }
        return bytes;
    }

    static void clear_stack(std::deque<Entry>& stack, std::size_t& bytes) noexcept {
        stack.clear();
        bytes = 0;
    }

    static void push(std::deque<Entry>& stack,
                     std::size_t& stack_bytes,
                     std::vector<Annotation> annotations) {
        const std::size_t bytes = estimate_bytes(annotations);
        stack.push_back({std::move(annotations), bytes});
        stack_bytes += bytes;
        while (stack.size() > 1 &&
               (stack.size() > kMaximumEntries ||
                stack_bytes > kMaximumBytesPerStack)) {
            stack_bytes -= stack.front().bytes;
            stack.pop_front();
        }
    }

    std::deque<Entry> undo_;
    std::deque<Entry> redo_;
    std::size_t undo_bytes_{};
    std::size_t redo_bytes_{};
};

}  // namespace airshot::overlay_detail
