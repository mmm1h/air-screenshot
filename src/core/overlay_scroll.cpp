#include "overlay_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace airshot::overlay_detail {

bool is_bitmap_static(const Bitmap& bmp1, const Bitmap& bmp2) {
    if (bmp1.empty() || bmp2.empty()) return false;
    if (bmp1.width != bmp2.width || bmp1.height != bmp2.height) return false;

    const int W = bmp1.width;
    const int H = bmp1.height;

    uint64_t diff = 0;
    const int step_y = 4;
    const int step_x = 4;
    int count = 0;

    for (int y = 0; y < H; y += step_y) {
        const auto* r1 = bmp1.row(y).data();
        const auto* r2 = bmp2.row(y).data();
        for (int x = 0; x < W; x += step_x) {
            const int idx = x * 4;
            diff += std::abs(static_cast<int>(r1[idx]) - static_cast<int>(r2[idx]));
            diff += std::abs(static_cast<int>(r1[idx + 1]) - static_cast<int>(r2[idx + 1]));
            diff += std::abs(static_cast<int>(r1[idx + 2]) - static_cast<int>(r2[idx + 2]));
            ++count;
        }
    }

    const double avg_diff = static_cast<double>(diff) / (count * 3.0);
    return avg_diff < 1.0;
}

int find_best_template_y(const Bitmap& frame, int direction) {
    const int W = frame.width;
    const int H = frame.height;
    const int th = 24;

    int start_y = H / 4;
    int end_y = 3 * H / 4 - th;

    const int edge_margin = std::max(40, H / 8);

    if (direction == 1) { // Downscroll (content moves up, so we want template from bottom, avoiding bottom edge)
        start_y = H / 2;
        end_y = H - th - edge_margin;
    } else if (direction == -1) { // Upscroll (content moves down, so we want template from top, avoiding top edge)
        start_y = edge_margin;
        end_y = H / 2 - th;
    }

    if (start_y > end_y) {
        start_y = H / 4;
        end_y = 3 * H / 4 - th;
    }

    int best_y = (start_y + end_y) / 2;
    uint64_t max_variance = 0;

    const int col_step = 8;

    for (int y = start_y; y <= end_y; y += 4) {
        uint64_t variance = 0;
        for (int row = 0; row < th - 1; ++row) {
            const auto* r1 = frame.row(y + row).data();
            const auto* r2 = frame.row(y + row + 1).data();
            for (int col = 0; col < W; col += col_step) {
                const int idx = col * 4;
                variance += std::abs(static_cast<int>(r1[idx]) - static_cast<int>(r2[idx]));
                variance += std::abs(static_cast<int>(r1[idx + 1]) - static_cast<int>(r2[idx + 1]));
                variance += std::abs(static_cast<int>(r1[idx + 2]) - static_cast<int>(r2[idx + 2]));
            }
        }
        if (variance > max_variance) {
            max_variance = variance;
            best_y = y;
        }
    }

    return best_y;
}



ScrollResult detect_scroll(const Bitmap& last_frame, const Bitmap& new_frame, int locked_direction) {
    if (last_frame.empty() || new_frame.empty()) return {};
    if (last_frame.width != new_frame.width || last_frame.height != new_frame.height) return {};

    const int W = last_frame.width;
    const int H = last_frame.height;
    const int th = 24;

    const int template_y = find_best_template_y(last_frame, locked_direction);
    const int col_step = 4;

    uint64_t variance = 0;
    {
        for (int row = 0; row < th - 1; ++row) {
            const auto* r1 = last_frame.row(template_y + row).data();
            const auto* r2 = last_frame.row(template_y + row + 1).data();
            for (int col = 0; col < W; col += col_step) {
                const int idx = col * 4;
                variance += std::abs(static_cast<int>(r1[idx]) - static_cast<int>(r2[idx]));
                variance += std::abs(static_cast<int>(r1[idx + 1]) - static_cast<int>(r2[idx + 1]));
                variance += std::abs(static_cast<int>(r1[idx + 2]) - static_cast<int>(r2[idx + 2]));
            }
        }
    }
    const double min_required_variance = (W / col_step) * th * 0.5;
    if (variance < min_required_variance) {
        return {false, 0, 0};
    }

    auto search_range = [&](int start_y, int end_y, int& out_best_y, double& out_avg_diff) -> bool {
        int best_y = -1;
        uint64_t min_sad = 0xFFFFFFFFFFFFFFFFULL;
        for (int y = start_y; y <= end_y; ++y) {
            uint64_t sad = 0;
            for (int row = 0; row < th; ++row) {
                const auto* last_row = last_frame.row(template_y + row).data();
                const auto* new_row = new_frame.row(y + row).data();
                for (int col = 0; col < W; col += col_step) {
                    const int idx = col * 4;
                    sad += std::abs(static_cast<int>(last_row[idx]) - static_cast<int>(new_row[idx]));
                    sad += std::abs(static_cast<int>(last_row[idx + 1]) - static_cast<int>(new_row[idx + 1]));
                    sad += std::abs(static_cast<int>(last_row[idx + 2]) - static_cast<int>(new_row[idx + 2]));
                }
            }
            if (sad < min_sad) {
                min_sad = sad;
                best_y = y;
            }
        }
        if (best_y == -1) return false;
        const double num_pixels = th * (W / col_step);
        out_avg_diff = static_cast<double>(min_sad) / (num_pixels * 3.0);
        out_best_y = best_y;
        return true;
    };

    const int max_disp = std::min(240, H / 2);
    int start_narrow = std::max(0, template_y - max_disp);
    int end_narrow = std::min(H - th, template_y + max_disp);

    int best_y = -1;
    double avg_diff = 100.0;

    // Stage 1: Narrow search
    if (search_range(start_narrow, end_narrow, best_y, avg_diff)) {
        if (avg_diff < 15.0) {
            const int diff = template_y - best_y;
            if (std::abs(diff) >= 2) {
                if (diff > 0 && diff < H - 20) {
                    return {true, 1, diff};
                } else if (diff < 0 && -diff < H - 20) {
                    return {true, -1, -diff};
                }
            } else {
                return {true, 0, 0}; // matched but no significant movement
            }
        }
    }

    // Stage 2: Fallback full search
    if (search_range(0, H - th, best_y, avg_diff)) {
        if (avg_diff < 15.0) {
            const int diff = template_y - best_y;
            if (std::abs(diff) >= 2) {
                if (diff > 0 && diff < H - 20) {
                    return {true, 1, diff};
                } else if (diff < 0 && -diff < H - 20) {
                    return {true, -1, -diff};
                }
            } else {
                return {true, 0, 0}; // matched but no significant movement
            }
        }
    }

    return {false, 0, 0};
}

void append_to_stitched(Bitmap& stitched, const Bitmap& new_frame, int d) {
    if (new_frame.empty() || d <= 0) return;

    const int W = new_frame.width;
    const int H = new_frame.height;
    const int old_h = stitched.height;
    const int new_h = old_h + d;

    stitched.pixels.resize(static_cast<std::size_t>(W) * new_h * 4U);
    stitched.height = new_h;

    for (int row = 0; row < d; ++row) {
        const int src_y = H - d + row;
        const int dest_y = old_h + row;
        const auto* src_row = new_frame.row(src_y).data();
        auto* dest_row = stitched.row(dest_y).data();
        std::memcpy(dest_row, src_row, static_cast<std::size_t>(W) * 4U);
    }
}

void prepend_to_stitched(Bitmap& stitched, const Bitmap& new_frame, int d) {
    if (new_frame.empty() || d <= 0) return;

    const int W = new_frame.width;
    const int old_h = stitched.height;
    const int new_h = old_h + d;

    stitched.pixels.resize(static_cast<std::size_t>(W) * new_h * 4U);
    stitched.height = new_h;

    std::memmove(
        stitched.pixels.data() + static_cast<std::size_t>(W) * d * 4U,
        stitched.pixels.data(),
        static_cast<std::size_t>(W) * old_h * 4U
    );

    for (int row = 0; row < d; ++row) {
        const auto* src_row = new_frame.row(row).data();
        auto* dest_row = stitched.row(row).data();
        std::memcpy(dest_row, src_row, static_cast<std::size_t>(W) * 4U);
    }
}



}  // namespace airshot::overlay_detail
