#include "overlay_helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace airshot::overlay_detail {

namespace {

constexpr int kBytesPerPixel = 4;
constexpr int kTemplateHeight = 24;
constexpr int kPixelSampleStep = 4;
constexpr int kCoarsePixelSampleStep = 8;
constexpr int kLocalSearchRadius = 240;
constexpr std::size_t kGlobalCoarseCandidates = 192;
constexpr std::size_t kGlobalCandidateBasins = 6;
constexpr double kStaticAverageDifference = 1.0;
constexpr double kMatchAverageDifference = 15.0;
constexpr double kMinimumConfidenceGap = 0.75;
constexpr int kOverlapPixelSampleStep = 8;
constexpr int kOverlapRowSampleStep = 4;
constexpr int kOverlapGoodPixelDifference = 24;
constexpr double kOverlapMaximumAverageDifference = 40.0;
constexpr double kOverlapMinimumGoodFraction = 0.55;
constexpr std::size_t kOverlapBandCount = 8;
constexpr std::size_t kMinimumOverlapSamples = 32;

bool checked_bitmap_bytes(int width, int height, std::size_t& bytes) noexcept {
    if (width <= 0 || height <= 0) return false;

    const auto width_value = static_cast<std::size_t>(width);
    const auto height_value = static_cast<std::size_t>(height);
    constexpr auto max_value = std::numeric_limits<std::size_t>::max();
    if (width_value > max_value / kBytesPerPixel) return false;

    const std::size_t row_bytes = width_value * kBytesPerPixel;
    if (height_value > max_value / row_bytes) return false;

    bytes = row_bytes * height_value;
    return true;
}

bool valid_bitmap(const Bitmap& bitmap) noexcept {
    return bitmap.valid();
}

bool stitched_dimensions_allowed(int width, int height, std::size_t& bytes) noexcept {
    return height <= kMaxScrollBitmapHeight &&
           checked_bitmap_bytes(width, height, bytes) &&
           bytes <= kMaxScrollBitmapBytes;
}

const std::uint8_t* pixel_row(const Bitmap& bitmap, int y) noexcept {
    const auto row_bytes = static_cast<std::size_t>(bitmap.width) * kBytesPerPixel;
    return bitmap.pixels.data() + static_cast<std::size_t>(y) * row_bytes;
}

std::uint8_t* pixel_row(Bitmap& bitmap, int y) noexcept {
    const auto row_bytes = static_cast<std::size_t>(bitmap.width) * kBytesPerPixel;
    return bitmap.pixels.data() + static_cast<std::size_t>(y) * row_bytes;
}

int adaptive_template_height(int frame_height) noexcept {
    if (frame_height <= 1) return 1;
    return std::min(kTemplateHeight, std::max(1, frame_height / 3));
}

std::uint64_t template_activity(const Bitmap& frame, int template_y, int template_height) {
    std::uint64_t activity = 0;
    for (int row = 0; row < template_height; ++row) {
        const auto* current = pixel_row(frame, template_y + row);
        if (row + 1 < template_height) {
            const auto* next = pixel_row(frame, template_y + row + 1);
            for (int column = 0; column < frame.width; column += kPixelSampleStep) {
                const auto index =
                    static_cast<std::size_t>(column) * kBytesPerPixel;
                for (int channel = 0; channel < 3; ++channel) {
                    activity += std::abs(
                        static_cast<int>(current[index + channel]) -
                        static_cast<int>(next[index + channel]));
                }
            }
        }

        for (int column = 0; column + 1 < frame.width; column += kPixelSampleStep) {
            const auto first =
                static_cast<std::size_t>(column) * kBytesPerPixel;
            const auto second =
                static_cast<std::size_t>(column + 1) * kBytesPerPixel;
            for (int channel = 0; channel < 3; ++channel) {
                activity += std::abs(
                    static_cast<int>(current[first + channel]) -
                    static_cast<int>(current[second + channel]));
            }
        }
    }
    return activity;
}

struct SearchResult {
    int y{-1};
    double average_difference{std::numeric_limits<double>::infinity()};
    double second_average_difference{std::numeric_limits<double>::infinity()};
};

struct ScoredCandidate {
    int y{-1};
    std::uint64_t sad{std::numeric_limits<std::uint64_t>::max()};
};

bool evaluate_template_candidate(const Bitmap& last_frame,
                                 const Bitmap& new_frame,
                                 int template_y,
                                 int template_height,
                                 int candidate_y,
                                 int pixel_sample_step,
                                 std::size_t& candidates_evaluated,
                                 ScoredCandidate& candidate) {
    if (candidates_evaluated >= kMaxScrollSearchCandidates) return false;

    ++candidates_evaluated;
    std::uint64_t sad = 0;
    for (int row = 0; row < template_height; ++row) {
        const auto* last_row = pixel_row(last_frame, template_y + row);
        const auto* new_row = pixel_row(new_frame, candidate_y + row);
        for (int column = 0; column < last_frame.width;
             column += pixel_sample_step) {
            const auto index =
                static_cast<std::size_t>(column) * kBytesPerPixel;
            for (int channel = 0; channel < 3; ++channel) {
                sad += std::abs(
                    static_cast<int>(last_row[index + channel]) -
                    static_cast<int>(new_row[index + channel]));
            }
        }
    }
    candidate = {candidate_y, sad};
    return true;
}

template <std::size_t Capacity>
SearchResult result_from_candidates(
    const std::array<ScoredCandidate, Capacity>& candidates,
    std::size_t candidate_count,
    int frame_width,
    int template_height,
    int pixel_sample_step) {
    SearchResult result;
    std::uint64_t best_sad = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t second_sad = std::numeric_limits<std::uint64_t>::max();

    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto& candidate = candidates[index];
        if (candidate.sad < best_sad) {
            second_sad = best_sad;
            best_sad = candidate.sad;
            result.y = candidate.y;
        } else if (candidate.sad < second_sad) {
            second_sad = candidate.sad;
        }
    }
    if (result.y < 0) return result;

    const auto sampled_columns =
        (static_cast<std::size_t>(frame_width) +
         static_cast<std::size_t>(pixel_sample_step) - 1U) /
        static_cast<std::size_t>(pixel_sample_step);
    const double sample_channels =
        static_cast<double>(sampled_columns) * template_height * 3.0;
    result.average_difference = static_cast<double>(best_sad) / sample_channels;
    if (second_sad != std::numeric_limits<std::uint64_t>::max()) {
        result.second_average_difference =
            static_cast<double>(second_sad) / sample_channels;
    }
    return result;
}

SearchResult search_template(const Bitmap& last_frame,
                             const Bitmap& new_frame,
                             int template_y,
                             int template_height,
                             int start_y,
                             int end_y,
                             int candidate_step,
                             int pixel_sample_step,
                             std::size_t& candidates_evaluated) {
    SearchResult result;
    std::uint64_t best_sad = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t second_sad = std::numeric_limits<std::uint64_t>::max();

    start_y = std::max(0, start_y);
    end_y = std::min(new_frame.height - template_height, end_y);
    if (start_y > end_y || candidate_step <= 0 || pixel_sample_step <= 0) return result;

    auto evaluate = [&](int candidate_y) {
        ScoredCandidate candidate;
        if (!evaluate_template_candidate(last_frame,
                                         new_frame,
                                         template_y,
                                         template_height,
                                         candidate_y,
                                         pixel_sample_step,
                                         candidates_evaluated,
                                         candidate)) {
            return false;
        }

        if (candidate.sad < best_sad) {
            second_sad = best_sad;
            best_sad = candidate.sad;
            result.y = candidate_y;
        } else if (candidate.sad < second_sad) {
            second_sad = candidate.sad;
        }
        return true;
    };

    int last_candidate = -1;
    for (std::int64_t candidate_y = start_y; candidate_y <= end_y;
         candidate_y += candidate_step) {
        last_candidate = static_cast<int>(candidate_y);
        if (!evaluate(last_candidate) ||
            candidate_y > static_cast<std::int64_t>(end_y) - candidate_step) {
            break;
        }
    }
    if (last_candidate != end_y &&
        candidates_evaluated < kMaxScrollSearchCandidates) {
        (void)evaluate(end_y);
    }

    if (result.y < 0) return result;

    const auto sampled_columns =
        (static_cast<std::size_t>(last_frame.width) +
         static_cast<std::size_t>(pixel_sample_step) - 1U) /
        static_cast<std::size_t>(pixel_sample_step);
    const double sample_channels =
        static_cast<double>(sampled_columns) * template_height * 3.0;
    result.average_difference = static_cast<double>(best_sad) / sample_channels;
    if (second_sad != std::numeric_limits<std::uint64_t>::max()) {
        result.second_average_difference = static_cast<double>(second_sad) / sample_channels;
    }
    return result;
}

SearchResult search_template_hierarchical(const Bitmap& last_frame,
                                          const Bitmap& new_frame,
                                          int template_y,
                                          int template_height,
                                          int start_y,
                                          int end_y,
                                          std::size_t& candidates_evaluated) {
    start_y = std::max(0, start_y);
    end_y = std::min(new_frame.height - template_height, end_y);
    if (start_y > end_y) {
        return {};
    }

    const std::int64_t candidate_count =
        static_cast<std::int64_t>(end_y) - start_y + 1;
    const int coarse_step = static_cast<int>(std::max<std::int64_t>(
        1,
        (candidate_count + static_cast<std::int64_t>(kGlobalCoarseCandidates) - 1) /
            static_cast<std::int64_t>(kGlobalCoarseCandidates)));

    // One endpoint may be added when the range is not divisible by the step.
    std::array<ScoredCandidate, kGlobalCoarseCandidates + 1U> coarse_candidates;
    std::size_t coarse_count = 0;
    const int coarse_pixel_step =
        coarse_step > 1 ? kCoarsePixelSampleStep : kPixelSampleStep;
    int last_candidate = -1;
    for (std::int64_t candidate_y = start_y; candidate_y <= end_y;
         candidate_y += coarse_step) {
        last_candidate = static_cast<int>(candidate_y);
        if (!evaluate_template_candidate(last_frame,
                                         new_frame,
                                         template_y,
                                         template_height,
                                         last_candidate,
                                         coarse_pixel_step,
                                         candidates_evaluated,
                                         coarse_candidates[coarse_count])) {
            break;
        }
        ++coarse_count;
        if (candidate_y > static_cast<std::int64_t>(end_y) - coarse_step) break;
    }
    if (last_candidate != end_y &&
        coarse_count < coarse_candidates.size() &&
        evaluate_template_candidate(last_frame,
                                    new_frame,
                                    template_y,
                                    template_height,
                                    end_y,
                                    coarse_pixel_step,
                                    candidates_evaluated,
                                    coarse_candidates[coarse_count])) {
        ++coarse_count;
    }
    if (coarse_count == 0 || coarse_step == 1) {
        return result_from_candidates(
            coarse_candidates,
            coarse_count,
            last_frame.width,
            template_height,
            coarse_pixel_step);
    }

    std::sort(coarse_candidates.begin(),
              coarse_candidates.begin() + coarse_count,
              [](const ScoredCandidate& left, const ScoredCandidate& right) {
                  return left.sad < right.sad ||
                         (left.sad == right.sad && left.y < right.y);
              });

    std::array<ScoredCandidate, kGlobalCandidateBasins> basins;
    std::size_t basin_count = 0;
    for (std::size_t index = 0;
         index < coarse_count && basin_count < basins.size();
         ++index) {
        const auto& candidate = coarse_candidates[index];
        bool separated = true;
        for (std::size_t basin_index = 0; basin_index < basin_count; ++basin_index) {
            if (std::abs(candidate.y - basins[basin_index].y) <=
                coarse_step * 2) {
                separated = false;
                break;
            }
        }
        if (separated) {
            basins[basin_count++] = candidate;
        }
    }
    if (basin_count == 0) return {};

    // Refining +/- the previous step with floor(step / 4) produces at most
    // 15 samples per basin (the worst case is step 7 -> 1).
    constexpr std::size_t kMaxRefinedCandidates =
        kGlobalCandidateBasins * 15U;
    std::array<ScoredCandidate, kMaxRefinedCandidates> refined_candidates;
    std::size_t refined_count = 0;
    int step = coarse_step;

    while (step > 1 && candidates_evaluated < kMaxScrollSearchCandidates) {
        const int next_step = std::max(1, step / 4);
        const int pixel_step =
            next_step > 1 ? kCoarsePixelSampleStep : kPixelSampleStep;
        refined_count = 0;
        bool refinement_complete = true;

        for (std::size_t basin_index = 0; basin_index < basin_count;
             ++basin_index) {
            const int refine_start = static_cast<int>(std::max<std::int64_t>(
                start_y,
                static_cast<std::int64_t>(basins[basin_index].y) - step));
            const int refine_end = static_cast<int>(std::min<std::int64_t>(
                end_y,
                static_cast<std::int64_t>(basins[basin_index].y) + step));
            ScoredCandidate best;
            int last_refined_y = -1;

            auto consider = [&](int candidate_y) {
                if (refined_count >= refined_candidates.size()) {
                    refinement_complete = false;
                    return false;
                }
                ScoredCandidate candidate;
                if (!evaluate_template_candidate(last_frame,
                                                 new_frame,
                                                 template_y,
                                                 template_height,
                                                 candidate_y,
                                                 pixel_step,
                                                 candidates_evaluated,
                                                 candidate)) {
                    refinement_complete = false;
                    return false;
                }
                refined_candidates[refined_count++] = candidate;
                if (candidate.sad < best.sad ||
                    (candidate.sad == best.sad && candidate.y < best.y)) {
                    best = candidate;
                }
                return true;
            };

            for (std::int64_t candidate_y = refine_start;
                 candidate_y <= refine_end;
                 candidate_y += next_step) {
                last_refined_y = static_cast<int>(candidate_y);
                if (!consider(last_refined_y) ||
                    candidate_y >
                        static_cast<std::int64_t>(refine_end) - next_step) {
                    break;
                }
            }
            if (last_refined_y != refine_end &&
                candidates_evaluated < kMaxScrollSearchCandidates) {
                (void)consider(refine_end);
            }
            if (best.y >= 0) {
                basins[basin_index] = best;
            }
            if (!refinement_complete) break;
        }
        if (!refinement_complete) return {};
        step = next_step;
    }

    if (step > 1) return {};
    return result_from_candidates(
        refined_candidates,
        refined_count,
        last_frame.width,
        template_height,
        step > 1 ? kCoarsePixelSampleStep : kPixelSampleStep);
}

bool confident_match(const SearchResult& result) noexcept {
    if (result.y < 0 || result.average_difference >= kMatchAverageDifference) return false;
    if (!std::isfinite(result.second_average_difference)) return true;
    return result.second_average_difference - result.average_difference >= kMinimumConfidenceGap;
}

struct OverlapEvidence {
    std::uint64_t difference{};
    std::size_t samples{};
    std::size_t good_samples{};
};

bool sufficient_overlap_evidence(const OverlapEvidence& evidence) noexcept {
    if (evidence.samples == 0) {
        return false;
    }
    const double average_difference =
        static_cast<double>(evidence.difference) /
        (static_cast<double>(evidence.samples) * 3.0);
    const double good_fraction =
        static_cast<double>(evidence.good_samples) /
        static_cast<double>(evidence.samples);
    return average_difference < kOverlapMaximumAverageDifference &&
           good_fraction >= kOverlapMinimumGoodFraction;
}

bool validate_scroll_overlap(const Bitmap& last_frame,
                             const Bitmap& new_frame,
                             int template_y,
                             int template_height,
                             int displacement) noexcept {
    const int offset = std::abs(displacement);
    const int overlap_height = last_frame.height - offset;
    if (overlap_height <= 0) {
        return false;
    }

    const int last_start = displacement > 0 ? offset : 0;
    const int new_start = displacement > 0 ? 0 : offset;
    std::array<OverlapEvidence, kOverlapBandCount> bands{};
    OverlapEvidence total;

    std::size_t sampled_row = 0;
    for (int overlap_y = 0; overlap_y < overlap_height;
         overlap_y += kOverlapRowSampleStep, ++sampled_row) {
        const int last_y = last_start + overlap_y;
        if (last_y >= template_y && last_y < template_y + template_height) {
            continue;
        }
        const int new_y = new_start + overlap_y;
        const auto* last_row = pixel_row(last_frame, last_y);
        const auto* new_row = pixel_row(new_frame, new_y);
        const std::size_t band_index = std::min<std::size_t>(
            kOverlapBandCount - 1,
            static_cast<std::size_t>(overlap_y) * kOverlapBandCount /
                static_cast<std::size_t>(overlap_height));
        OverlapEvidence& band = bands[band_index];
        const int first_x = static_cast<int>(
            (sampled_row * 3U) % static_cast<std::size_t>(kOverlapPixelSampleStep));
        for (int x = first_x; x < last_frame.width; x += kOverlapPixelSampleStep) {
            const std::size_t pixel =
                static_cast<std::size_t>(x) * kBytesPerPixel;
            const int difference =
                std::abs(static_cast<int>(last_row[pixel]) -
                         static_cast<int>(new_row[pixel])) +
                std::abs(static_cast<int>(last_row[pixel + 1]) -
                         static_cast<int>(new_row[pixel + 1])) +
                std::abs(static_cast<int>(last_row[pixel + 2]) -
                         static_cast<int>(new_row[pixel + 2]));
            total.difference += static_cast<std::uint64_t>(difference);
            band.difference += static_cast<std::uint64_t>(difference);
            ++total.samples;
            ++band.samples;
            if (difference <= kOverlapGoodPixelDifference * 3) {
                ++total.good_samples;
                ++band.good_samples;
            }
        }
    }

    if (total.samples < kMinimumOverlapSamples ||
        !sufficient_overlap_evidence(total)) {
        return false;
    }

    std::size_t populated_bands = 0;
    std::size_t matching_bands = 0;
    for (const auto& band : bands) {
        if (band.samples == 0) {
            continue;
        }
        ++populated_bands;
        if (sufficient_overlap_evidence(band)) {
            ++matching_bands;
        }
    }
    return populated_bands >= 2 &&
           matching_bands >= (populated_bands + 1U) / 2U;
}

bool copy_strip(const Bitmap& source, int source_y, int row_count, Bitmap& output) {
    std::size_t output_bytes = 0;
    if (!valid_bitmap(source) || source_y < 0 || row_count <= 0 ||
        source_y > source.height - row_count ||
        !checked_bitmap_bytes(source.width, row_count, output_bytes)) {
        return false;
    }

    try {
        Bitmap strip;
        strip.width = source.width;
        strip.height = row_count;
        strip.pixels.resize(output_bytes);
        std::memcpy(
            strip.pixels.data(),
            pixel_row(source, source_y),
            output_bytes);
        output = std::move(strip);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

}  // namespace

bool scroll_bitmap_fits_budget(int width, int height) noexcept {
    std::size_t bytes = 0;
    return stitched_dimensions_allowed(width, height, bytes);
}

bool replace_scroll_resume_baseline(
    Bitmap& baseline,
    Bitmap candidate) noexcept {
    if (!baseline.valid() || !candidate.valid() ||
        baseline.width != candidate.width ||
        baseline.height != candidate.height) {
        return false;
    }
    baseline = std::move(candidate);
    return true;
}

bool is_bitmap_static(const Bitmap& bmp1, const Bitmap& bmp2) {
    if (!valid_bitmap(bmp1) || !valid_bitmap(bmp2)) return false;
    if (bmp1.width != bmp2.width || bmp1.height != bmp2.height) return false;

    const int W = bmp1.width;
    const int H = bmp1.height;

    uint64_t diff = 0;
    constexpr int step_y = 4;
    constexpr int step_x = 4;
    std::size_t count = 0;

    for (int y = 0; y < H; y += step_y) {
        const auto* r1 = pixel_row(bmp1, y);
        const auto* r2 = pixel_row(bmp2, y);
        for (int x = 0; x < W; x += step_x) {
            const auto idx = static_cast<std::size_t>(x) * kBytesPerPixel;
            diff += std::abs(static_cast<int>(r1[idx]) - static_cast<int>(r2[idx]));
            diff += std::abs(static_cast<int>(r1[idx + 1]) - static_cast<int>(r2[idx + 1]));
            diff += std::abs(static_cast<int>(r1[idx + 2]) - static_cast<int>(r2[idx + 2]));
            ++count;
        }
    }

    const double avg_diff = static_cast<double>(diff) / (count * 3.0);
    return avg_diff < kStaticAverageDifference;
}

int find_best_template_y(const Bitmap& frame, int direction) {
    if (!valid_bitmap(frame)) return 0;

    const int H = frame.height;
    const int template_height = adaptive_template_height(H);
    const int max_template_y = H - template_height;

    int start_y = max_template_y / 4;
    int end_y = 3 * max_template_y / 4;
    const int edge_margin = std::min(max_template_y / 3, std::max(1, H / 8));

    if (direction > 0) {
        start_y = max_template_y / 2;
        end_y = max_template_y - edge_margin;
    } else if (direction < 0) {
        start_y = edge_margin;
        end_y = max_template_y / 2;
    }

    if (start_y > end_y) {
        start_y = 0;
        end_y = max_template_y;
    }
    start_y = std::clamp(start_y, 0, max_template_y);
    end_y = std::clamp(end_y, start_y, max_template_y);

    int best_y = (start_y + end_y) / 2;
    std::uint64_t max_activity = 0;

    auto consider = [&](int y, std::uint64_t activity) {
        if (activity > max_activity) {
            max_activity = activity;
            best_y = y;
        }
    };

    try {
        std::vector<std::uint64_t> horizontal_prefix(
            static_cast<std::size_t>(H) + 1U);
        std::vector<std::uint64_t> vertical_prefix(
            static_cast<std::size_t>(H) + 1U);

        for (int y = 0; y < H; ++y) {
            const auto* current = pixel_row(frame, y);
            std::uint64_t horizontal_activity = 0;
            std::uint64_t vertical_activity = 0;
            const auto* next = y + 1 < H ? pixel_row(frame, y + 1) : nullptr;
            for (int column = 0; column < frame.width;
                 column += kPixelSampleStep) {
                const auto index =
                    static_cast<std::size_t>(column) * kBytesPerPixel;
                if (next) {
                    for (int channel = 0; channel < 3; ++channel) {
                        vertical_activity += std::abs(
                            static_cast<int>(current[index + channel]) -
                            static_cast<int>(next[index + channel]));
                    }
                }
                if (column + 1 < frame.width) {
                    const auto adjacent =
                        static_cast<std::size_t>(column + 1) * kBytesPerPixel;
                    for (int channel = 0; channel < 3; ++channel) {
                        horizontal_activity += std::abs(
                            static_cast<int>(current[index + channel]) -
                            static_cast<int>(current[adjacent + channel]));
                    }
                }
            }
            const auto prefix_index = static_cast<std::size_t>(y) + 1U;
            horizontal_prefix[prefix_index] =
                horizontal_prefix[prefix_index - 1U] + horizontal_activity;
            vertical_prefix[prefix_index] =
                vertical_prefix[prefix_index - 1U] + vertical_activity;
        }

        for (int y = start_y; y <= end_y; y += 4) {
            const auto begin = static_cast<std::size_t>(y);
            const auto horizontal_end =
                static_cast<std::size_t>(y + template_height);
            const auto vertical_end =
                static_cast<std::size_t>(y + template_height - 1);
            const auto activity =
                horizontal_prefix[horizontal_end] - horizontal_prefix[begin] +
                vertical_prefix[vertical_end] - vertical_prefix[begin];
            consider(y, activity);
        }
    } catch (const std::bad_alloc&) {
        for (int y = start_y; y <= end_y; y += 4) {
            consider(y, template_activity(frame, y, template_height));
        }
    } catch (const std::length_error&) {
        for (int y = start_y; y <= end_y; y += 4) {
            consider(y, template_activity(frame, y, template_height));
        }
    }
    return best_y;
}

ScrollResult detect_scroll(const Bitmap& last_frame,
                           const Bitmap& new_frame,
                           int locked_direction,
                           ScrollSearchStats* stats) {
    if (stats) {
        *stats = {};
    }
    if (!valid_bitmap(last_frame) || !valid_bitmap(new_frame)) return {};
    if (last_frame.width != new_frame.width || last_frame.height != new_frame.height) return {};
    if (is_bitmap_static(last_frame, new_frame)) return {true, 0, 0};

    const int H = last_frame.height;
    const int template_height = adaptive_template_height(H);
    const int template_y = find_best_template_y(last_frame, locked_direction);
    if (template_activity(last_frame, template_y, template_height) == 0) return {};

    const int max_displacement = std::min(kLocalSearchRadius, std::max(1, H / 2));
    const int max_candidate_y = H - template_height;
    std::size_t candidates_evaluated = 0;
    auto match = search_template(
        last_frame,
        new_frame,
        template_y,
        template_height,
        template_y - max_displacement,
        template_y + max_displacement,
        1,
        kPixelSampleStep,
        candidates_evaluated);

    if (!confident_match(match) &&
        (template_y - max_displacement > 0 ||
         template_y + max_displacement < max_candidate_y)) {
        match = search_template_hierarchical(
            last_frame,
            new_frame,
            template_y,
            template_height,
            0,
            max_candidate_y,
            candidates_evaluated);
    }
    if (stats) {
        stats->candidates_evaluated = candidates_evaluated;
    }
    if (!confident_match(match)) return {};

    const int displacement = template_y - match.y;
    if (!validate_scroll_overlap(
            last_frame, new_frame, template_y, template_height, displacement)) {
        return {};
    }
    if (std::abs(displacement) < 2) return {true, 0, 0};
    if (displacement > 0 && displacement < H) return {true, 1, displacement};
    if (displacement < 0 && -displacement < H) return {true, -1, -displacement};
    return {};
}

ScrollStitcher::ScrollStitcher(Bitmap initial_frame)
    : initial_frame_(std::move(initial_frame)) {
    std::size_t bytes = 0;
    if (!valid_bitmap(initial_frame_) ||
        !stitched_dimensions_allowed(initial_frame_.width, initial_frame_.height, bytes)) {
        return;
    }

    width_ = initial_frame_.width;
    height_ = initial_frame_.height;
    valid_ = true;
}

bool ScrollStitcher::valid() const noexcept {
    return valid_;
}

int ScrollStitcher::width() const noexcept {
    return width_;
}

int ScrollStitcher::height() const noexcept {
    return height_;
}

int ScrollStitcher::direction() const noexcept {
    return direction_;
}

StitchStatus ScrollStitcher::append(const Bitmap& new_frame, int offset) {
    return add_strip(new_frame, offset, 1);
}

StitchStatus ScrollStitcher::prepend(const Bitmap& new_frame, int offset) {
    return add_strip(new_frame, offset, -1);
}

StitchStatus ScrollStitcher::add_strip(const Bitmap& new_frame, int offset, int direction) {
    if (!valid_ || !valid_bitmap(new_frame) || offset <= 0 || offset > new_frame.height) {
        return StitchStatus::invalid_input;
    }
    if (new_frame.width != width_) return StitchStatus::dimension_mismatch;
    if (direction_ != 0 && direction_ != direction) return StitchStatus::direction_mismatch;
    if (height_ > kMaxScrollBitmapHeight - offset) return StitchStatus::limit_reached;

    const int new_height = height_ + offset;
    std::size_t total_bytes = 0;
    if (!stitched_dimensions_allowed(width_, new_height, total_bytes)) {
        return StitchStatus::limit_reached;
    }

    Bitmap strip;
    const int source_y = direction > 0 ? new_frame.height - offset : 0;
    if (!copy_strip(new_frame, source_y, offset, strip)) {
        return StitchStatus::allocation_failed;
    }

    try {
        auto& strips = direction > 0 ? appended_strips_ : prepended_strips_;
        strips.push_back(std::move(strip));
    } catch (const std::bad_alloc&) {
        return StitchStatus::allocation_failed;
    } catch (const std::length_error&) {
        return StitchStatus::allocation_failed;
    }

    direction_ = direction;
    height_ = new_height;
    return StitchStatus::success;
}

StitchStatus ScrollStitcher::materialize(Bitmap& output) const {
    if (!valid_) return StitchStatus::invalid_input;

    std::size_t total_bytes = 0;
    if (!stitched_dimensions_allowed(width_, height_, total_bytes)) {
        return StitchStatus::limit_reached;
    }

    try {
        Bitmap result;
        result.width = width_;
        result.height = height_;
        result.pixels.resize(total_bytes);

        const std::size_t row_bytes = static_cast<std::size_t>(width_) * kBytesPerPixel;
        std::size_t destination_offset = 0;
        auto copy_bitmap = [&](const Bitmap& bitmap) {
            const std::size_t bytes = row_bytes * static_cast<std::size_t>(bitmap.height);
            std::memcpy(result.pixels.data() + destination_offset, bitmap.pixels.data(), bytes);
            destination_offset += bytes;
        };

        for (auto strip = prepended_strips_.rbegin(); strip != prepended_strips_.rend(); ++strip) {
            copy_bitmap(*strip);
        }
        copy_bitmap(initial_frame_);
        for (const auto& strip : appended_strips_) {
            copy_bitmap(strip);
        }

        if (destination_offset != total_bytes) return StitchStatus::invalid_input;
        output = std::move(result);
        return StitchStatus::success;
    } catch (const std::bad_alloc&) {
        return StitchStatus::allocation_failed;
    } catch (const std::length_error&) {
        return StitchStatus::allocation_failed;
    }
}

void append_to_stitched(Bitmap& stitched, const Bitmap& new_frame, int d) {
    if (&stitched == &new_frame ||
        !valid_bitmap(stitched) || !valid_bitmap(new_frame) || d <= 0 ||
        d > new_frame.height || stitched.width != new_frame.width ||
        stitched.height > kMaxScrollBitmapHeight - d) {
        return;
    }

    const int old_height = stitched.height;
    const int new_height = old_height + d;
    std::size_t new_bytes = 0;
    if (!stitched_dimensions_allowed(stitched.width, new_height, new_bytes)) return;

    try {
        stitched.pixels.resize(new_bytes);
        const std::size_t row_bytes =
            static_cast<std::size_t>(stitched.width) * kBytesPerPixel;
        std::memcpy(
            pixel_row(stitched, old_height),
            pixel_row(new_frame, new_frame.height - d),
            row_bytes * static_cast<std::size_t>(d));
        stitched.height = new_height;
    } catch (const std::bad_alloc&) {
    } catch (const std::length_error&) {
    }
}

void prepend_to_stitched(Bitmap& stitched, const Bitmap& new_frame, int d) {
    if (&stitched == &new_frame ||
        !valid_bitmap(stitched) || !valid_bitmap(new_frame) || d <= 0 ||
        d > new_frame.height || stitched.width != new_frame.width ||
        stitched.height > kMaxScrollBitmapHeight - d) {
        return;
    }

    const int old_height = stitched.height;
    const int new_height = old_height + d;
    std::size_t new_bytes = 0;
    if (!stitched_dimensions_allowed(stitched.width, new_height, new_bytes)) return;

    try {
        stitched.pixels.resize(new_bytes);
        const std::size_t row_bytes =
            static_cast<std::size_t>(stitched.width) * kBytesPerPixel;
        std::memmove(
            pixel_row(stitched, d),
            pixel_row(stitched, 0),
            row_bytes * static_cast<std::size_t>(old_height));
        std::memcpy(
            pixel_row(stitched, 0),
            pixel_row(new_frame, 0),
            row_bytes * static_cast<std::size_t>(d));
        stitched.height = new_height;
    } catch (const std::bad_alloc&) {
    } catch (const std::length_error&) {
    }
}

}  // namespace airshot::overlay_detail
