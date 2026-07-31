#include "airshot/bitmap.h"
#include "airshot/capture.h"
#include "airshot/pin_layout.h"
#include "airshot/command.h"
#include "airshot/config.h"
#include "airshot/ocr.h"
#include "airshot/portable.h"
#include "airshot/region_policy.h"
#include "airshot/output.h"

#include "../src/core/overlay_types.h"
#include "../src/core/overlay_annotation_history.h"

#include <winrt/base.h>

#include <cmath>
#include <iostream>
#include <fstream>

namespace {

int failures = 0;

void expect(bool condition, std::wstring_view message) {
    if (!condition) {
        ++failures;
        std::wcerr << L"FAIL: " << message << L'\n';
    }
}

void test_rect_and_bitmap() {
    const airshot::RectI value{10, 20, -5, 3};
    const auto normalized = value.normalized();
    expect(normalized.left == -5 && normalized.top == 3 && normalized.right == 10 && normalized.bottom == 20,
           L"RectI normalization");
    const auto overlap = airshot::intersect({-10, -10, 10, 10}, {0, 0, 20, 20});
    expect(overlap && overlap->left == 0 && overlap->top == 0 && overlap->right == 10 && overlap->bottom == 10,
           L"negative-coordinate intersection");

    airshot::Bitmap source(4, 4);
    for (std::size_t index = 0; index < source.pixels.size(); ++index) {
        source.pixels[index] = static_cast<std::uint8_t>(index);
    }
    airshot::Bitmap target(2, 2);
    airshot::blit(source, {1, 1, 3, 3}, target, POINT{0, 0});
    expect(target.pixels[0] == source.pixels[20], L"bitmap blit");
    const auto cropped = airshot::crop(source, {1, 1, 3, 3});
    expect(cropped.width == 2 && cropped.height == 2 && cropped.pixels == target.pixels, L"bitmap crop");

    // Test rotation and flipping
    airshot::Bitmap rot_source(2, 3);
    rot_source.pixels = {
        1, 0, 0, 255,   2, 0, 0, 255,
        3, 0, 0, 255,   4, 0, 0, 255,
        5, 0, 0, 255,   6, 0, 0, 255
    };
    const auto rot_cw = airshot::rotate_90_cw(rot_source);
    expect(rot_cw.width == 3 && rot_cw.height == 2, L"rotate_90_cw dimensions");
    expect(rot_cw.pixels[0] == 5 && rot_cw.pixels[4] == 3 && rot_cw.pixels[8] == 1, L"rotate_90_cw row 0");
    expect(rot_cw.pixels[12] == 6 && rot_cw.pixels[16] == 4 && rot_cw.pixels[20] == 2, L"rotate_90_cw row 1");

    const auto rot_ccw = airshot::rotate_90_ccw(rot_source);
    expect(rot_ccw.width == 3 && rot_ccw.height == 2, L"rotate_90_ccw dimensions");
    expect(rot_ccw.pixels[0] == 2 && rot_ccw.pixels[4] == 4 && rot_ccw.pixels[8] == 6, L"rotate_90_ccw row 0");
    expect(rot_ccw.pixels[12] == 1 && rot_ccw.pixels[16] == 3 && rot_ccw.pixels[20] == 5, L"rotate_90_ccw row 1");

    const auto flip_h = airshot::flip_horizontal(rot_source);
    expect(flip_h.width == 2 && flip_h.height == 3, L"flip_horizontal dimensions");
    expect(flip_h.pixels[0] == 2 && flip_h.pixels[4] == 1, L"flip_horizontal row 0");
    expect(flip_h.pixels[8] == 4 && flip_h.pixels[12] == 3, L"flip_horizontal row 1");
    expect(flip_h.pixels[16] == 6 && flip_h.pixels[20] == 5, L"flip_horizontal row 2");

    const auto flip_v = airshot::flip_vertical(rot_source);
    expect(flip_v.width == 2 && flip_v.height == 3, L"flip_vertical dimensions");
    expect(flip_v.pixels[0] == 5 && flip_v.pixels[4] == 6, L"flip_vertical row 0");
    expect(flip_v.pixels[8] == 3 && flip_v.pixels[12] == 4, L"flip_vertical row 1");
    expect(flip_v.pixels[16] == 1 && flip_v.pixels[20] == 2, L"flip_vertical row 2");

    // Test blur_circle
    airshot::Bitmap blur_source(5, 5);
    std::fill(blur_source.pixels.begin(), blur_source.pixels.end(), static_cast<uint8_t>(0));
    blur_source.pixels[(2 * 5 + 2) * 4] = 200;
    blur_source.pixels[(2 * 5 + 2) * 4 + 1] = 200;
    blur_source.pixels[(2 * 5 + 2) * 4 + 2] = 200;
    blur_source.pixels[(2 * 5 + 2) * 4 + 3] = 255;
    airshot::blur_circle(blur_source, POINT{2, 2}, 2, 1);
    expect(blur_source.pixels[(2 * 5 + 2) * 4] < 200 && blur_source.pixels[(2 * 5 + 2) * 4] > 0, L"blur_circle center pixel blurred");
    expect(blur_source.pixels[(2 * 5 + 1) * 4] > 0, L"blur_circle spreads to neighbors");

    const auto temp_png = std::filesystem::temp_directory_path() / L"airshot-test-save.png";
    std::wstring save_error;
    expect(airshot::save_png(source, temp_png, &save_error), L"save_png works");
    expect(std::filesystem::exists(temp_png), L"saved png file exists");
    std::error_code ignored;
    std::filesystem::remove(temp_png, ignored);
}

bool same_rect(const airshot::RectI& first, const airshot::RectI& second) {
    return first.left == second.left && first.top == second.top &&
           first.right == second.right && first.bottom == second.bottom;
}

void test_window_candidate_hierarchy() {
    const HWND first_root = reinterpret_cast<HWND>(1);
    const HWND second_root = reinterpret_cast<HWND>(2);
    const std::vector<airshot::WindowCandidate> candidates{
        {first_root, {0, 0, 100, 100}, first_root, nullptr, 0},
        {reinterpret_cast<HWND>(3), {10, 10, 90, 90}, first_root, first_root, 1},
        {reinterpret_cast<HWND>(4), {20, 20, 40, 40}, first_root, reinterpret_cast<HWND>(3), 2},
        // A smaller overlapping sibling must not become the parent when the
        // user scrolls up the hierarchy from candidate 4.
        {reinterpret_cast<HWND>(5), {22, 22, 35, 35}, first_root, first_root, 1},
        {second_root, {15, 15, 120, 120}, second_root, nullptr, 0},
        {reinterpret_cast<HWND>(6), {20, 20, 110, 110}, second_root, second_root, 1},
    };

    const auto deepest = airshot::window_candidate_at_point(
        candidates,
        {25, 25});
    expect(
        deepest && deepest->handle == reinterpret_cast<HWND>(4),
        L"window candidate prefers the smallest deepest control in the topmost root");
    const auto parent = airshot::window_candidate_at_point(
        candidates,
        {25, 25},
        1);
    expect(
        parent && parent->handle == reinterpret_cast<HWND>(3),
        L"window candidate wheel cycle follows the selected control's real parent");
    const auto root = airshot::window_candidate_at_point(
        candidates,
        {25, 25},
        99);
    expect(
        root && root->handle == first_root,
        L"window candidate ancestor cycle clamps to top-level root");
    const auto lower_root = airshot::window_candidate_at_point(
        candidates,
        {105, 105});
    expect(
        lower_root &&
            lower_root->handle == reinterpret_cast<HWND>(6),
        L"window candidate advances to the next visible root");
    expect(
        !airshot::window_candidate_at_point(candidates, {-1, -1}),
        L"window candidate rejects points outside all candidates");
}

void test_pin_recovery_layout() {
    const airshot::RectI work{0, 0, 1920, 1080};
    const auto recovered =
        airshot::recover_pin_bounds(
            {2000, 1100, 2300, 1300},
            work);
    expect(
        recovered.left == 1620 &&
            recovered.top == 880 &&
            recovered.right == 1920 &&
            recovered.bottom == 1080,
        L"off-screen pin is restored fully inside the work area");

    const auto oversized =
        airshot::recover_pin_bounds(
            {2000, 2000, 5000, 4000},
            work);
    expect(
        oversized.left == 1872 &&
            oversized.top == 1032 &&
            oversized.width() == 3000 &&
            oversized.height() == 2000,
        L"oversized pin keeps a draggable edge visible");

    const auto negative_monitor =
        airshot::recover_pin_bounds(
            {-2500, -100, -2200, 100},
            {-1920, 0, 0, 1080});
    expect(
        negative_monitor.left == -1920 &&
            negative_monitor.top == 0 &&
            negative_monitor.right == -1620 &&
            negative_monitor.bottom == 200,
        L"pin recovery supports negative monitor coordinates");

    expect(
        airshot::fit_pin_scale(640, 480, work) == 1.0,
        L"small pins keep their original scale");
    expect(
        std::abs(
            airshot::fit_pin_scale(4000, 2000, work) -
            0.3936) < 0.0001,
        L"large pins fit within the configured work-area coverage");
    expect(
        airshot::fit_pin_scale(0, 2000, work) == 1.0,
        L"invalid pin dimensions keep a safe default scale");
}

void test_serial_counter_is_scoped_to_capture_session() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::Tool;

    std::vector<Annotation> first_capture;
    expect(
        airshot::overlay_detail::next_serial_number(first_capture) == 1,
           L"first serial in a capture session starts at one");
    Annotation first;
    first.tool = Tool::serial;
    first.serial = 1;
    first_capture.push_back(first);
    expect(
        airshot::overlay_detail::next_serial_number(first_capture) == 2,
           L"serials increment within one capture session");

    Annotation third = first;
    third.serial = 3;
    first_capture.push_back(third);
    airshot::overlay_detail::renumber_serial_annotations(first_capture);
    expect(
        first_capture[0].serial == 1 &&
            first_capture[1].serial == 2 &&
            airshot::overlay_detail::next_serial_number(first_capture) == 3,
        L"deleting a middle serial keeps numbering continuous");

    std::vector<Annotation> next_capture;
    expect(
        airshot::overlay_detail::next_serial_number(next_capture) == 1,
           L"a new capture session resets serial numbering to one");
}

void test_annotation_geometry_and_history() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::AnnotationHistory;
    using airshot::overlay_detail::Tool;

    expect(
        airshot::overlay_detail::tool_cursor_radius(
            Tool::pen,
            12.0F) == 6.0F &&
            airshot::overlay_detail::tool_cursor_radius(
                Tool::mosaic,
                12.0F) == 42.0F &&
            airshot::overlay_detail::tool_cursor_radius(
                Tool::highlight,
                12.0F) == 19.0F &&
            airshot::overlay_detail::tool_cursor_radius(
                Tool::eraser,
                3.0F) == 8.0F,
        L"brush cursor matches each tool's real editing footprint");

    Annotation mosaic_footprint;
    mosaic_footprint.tool = Tool::mosaic;
    mosaic_footprint.width = 12.0F;
    mosaic_footprint.points = {{50, 50}};
    const auto mosaic_bounds =
        airshot::overlay_detail::annotation_bounds(mosaic_footprint);
    expect(
        airshot::overlay_detail::annotation_visual_radius(
            mosaic_footprint) == 42.0F &&
            mosaic_bounds.left == 8 && mosaic_bounds.top == 8 &&
            mosaic_bounds.right == 93 && mosaic_bounds.bottom == 93,
        L"annotation bounds and cursor share the mosaic footprint");

    Annotation degenerate_rectangle;
    degenerate_rectangle.tool = Tool::rectangle;
    degenerate_rectangle.start = {10, 10};
    degenerate_rectangle.end = {11, 30};
    Annotation valid_rectangle = degenerate_rectangle;
    valid_rectangle.end = {12, 12};
    Annotation short_arrow;
    short_arrow.tool = Tool::arrow;
    short_arrow.start = {0, 0};
    short_arrow.end = {2, 0};
    Annotation valid_arrow = short_arrow;
    valid_arrow.end = {3, 0};
    expect(
        !airshot::overlay_detail::annotation_is_committable(
            degenerate_rectangle) &&
            airshot::overlay_detail::annotation_is_committable(
                valid_rectangle) &&
            !airshot::overlay_detail::annotation_is_committable(
                short_arrow) &&
            airshot::overlay_detail::annotation_is_committable(
                valid_arrow),
        L"degenerate boxes and short arrows are filtered before history commit");

    expect(
        airshot::overlay_detail::interaction_settle_mode(
            airshot::overlay_detail::InteractionCommand::copy) ==
                airshot::overlay_detail::InteractionSettleMode::commit &&
            airshot::overlay_detail::interaction_settle_mode(
                airshot::overlay_detail::InteractionCommand::save) ==
                airshot::overlay_detail::InteractionSettleMode::commit &&
            airshot::overlay_detail::interaction_settle_mode(
                airshot::overlay_detail::InteractionCommand::ocr) ==
                airshot::overlay_detail::InteractionSettleMode::commit &&
            airshot::overlay_detail::interaction_settle_mode(
                airshot::overlay_detail::InteractionCommand::undo) ==
                airshot::overlay_detail::InteractionSettleMode::cancel &&
            airshot::overlay_detail::interaction_settle_mode(
                airshot::overlay_detail::InteractionCommand::redo) ==
                airshot::overlay_detail::InteractionSettleMode::cancel &&
            airshot::overlay_detail::interaction_settle_mode(
                airshot::overlay_detail::InteractionCommand::close) ==
                airshot::overlay_detail::InteractionSettleMode::cancel,
        L"output commands commit active gestures while history and close cancel them");

    const auto square =
        airshot::overlay_detail::constrained_annotation_geometry(
            Tool::rectangle,
            POINT{10, 10},
            POINT{14, 20},
            true,
            false);
    expect(
        square.start.x == 10 && square.start.y == 10 &&
            square.end.x == 20 && square.end.y == 20,
        L"Shift constrains rectangles to a square");

    const auto centered =
        airshot::overlay_detail::constrained_annotation_geometry(
            Tool::ellipse,
            POINT{20, 20},
            POINT{24, 26},
            true,
            true);
    expect(
        centered.start.x == 14 && centered.start.y == 14 &&
            centered.end.x == 26 && centered.end.y == 26,
        L"Alt draws constrained shapes from their center");

    const auto horizontal =
        airshot::overlay_detail::constrained_annotation_geometry(
            Tool::arrow,
            POINT{0, 0},
            POINT{10, 3},
            true,
            false);
    expect(
        horizontal.end.y == 0,
        L"Shift snaps lines and arrows to 45 degree increments");

    const auto centered_near_edge =
        airshot::overlay_detail::fit_annotation_geometry_to_canvas(
            airshot::overlay_detail::constrained_annotation_geometry(
                Tool::rectangle,
                POINT{10, 50},
                POINT{60, 80},
                false,
                true),
            POINT{10, 50},
            100,
            100,
            true,
            false);
    expect(
        centered_near_edge.start.x == 0 &&
            centered_near_edge.end.x == 20 &&
            centered_near_edge.start.y == 20 &&
            centered_near_edge.end.y == 80,
        L"Alt-centered shapes clamp each axis without distorting the other");

    const auto square_selection =
        airshot::overlay_detail::constrained_selection_rect(
            POINT{10, 10},
            POINT{40, 25},
            airshot::RectI{0, 0, 100, 100},
            true,
            false);
    expect(
        square_selection.left == 10 && square_selection.top == 10 &&
            square_selection.right == 40 && square_selection.bottom == 40,
        L"Shift constrains a new capture selection to a stable square");

    const auto centered_selection =
        airshot::overlay_detail::constrained_selection_rect(
            POINT{50, 50},
            POINT{70, 60},
            airshot::RectI{0, 0, 100, 100},
            false,
            true);
    expect(
        centered_selection.left == 30 && centered_selection.top == 40 &&
            centered_selection.right == 70 && centered_selection.bottom == 60,
        L"Alt expands a new capture selection from its center");

    const auto aspect_selection =
        airshot::overlay_detail::resize_selection_from_corner(
            airshot::RectI{10, 10, 50, 30},
            airshot::overlay_detail::DragMode::bottom_right,
            POINT{70, 35},
            airshot::RectI{0, 0, 100, 100},
            true,
            false);
    expect(
        aspect_selection.left == 10 && aspect_selection.top == 10 &&
            aspect_selection.right == 70 && aspect_selection.bottom == 40,
        L"Shift preserves the original ratio while resizing a capture corner");

    const auto centered_resize =
        airshot::overlay_detail::resize_selection_from_corner(
            airshot::RectI{20, 20, 60, 40},
            airshot::overlay_detail::DragMode::bottom_right,
            POINT{70, 45},
            airshot::RectI{0, 0, 100, 100},
            false,
            true);
    expect(
        centered_resize.left == 10 && centered_resize.top == 15 &&
            centered_resize.right == 70 && centered_resize.bottom == 45,
        L"Alt resizes an existing capture corner around the selection center");

    const std::vector<POINT> long_stroke{{0, 0}, {100, 0}};
    const auto capped_stroke =
        airshot::overlay_detail::resample_polyline(long_stroke, 1.0, 2);
    expect(
        capped_stroke.size() == 2 &&
            capped_stroke.front().x == 0 &&
            capped_stroke.back().x == 100,
        L"bounded stroke resampling always preserves the final endpoint");

    Annotation rectangle;
    rectangle.tool = Tool::rectangle;
    rectangle.start = {10, 10};
    rectangle.end = {30, 30};
    std::vector<Annotation> annotations;
    AnnotationHistory history;
    history.record(annotations);
    annotations.push_back(rectangle);
    expect(
        history.can_undo() && history.undo(annotations) &&
            annotations.empty() && history.can_redo(),
        L"annotation creation participates in undo");
    expect(
        history.redo(annotations) && annotations.size() == 1,
        L"annotation creation participates in redo");

    history.record(annotations);
    airshot::overlay_detail::translate_annotation(
        annotations.front(),
        15,
        5);
    expect(
        history.undo(annotations) &&
            annotations.front().start.x == 10 &&
            annotations.front().start.y == 10,
        L"annotation movement is restored atomically");

    const POINT clamped =
        airshot::overlay_detail::clamp_annotation_translation(
            rectangle,
            -100,
            100,
            100,
            100);
    expect(
        clamped.x == -8 && clamped.y == 68,
        L"annotation movement keeps the full stroke inside the capture");

    const auto handles =
        airshot::overlay_detail::annotation_control_handles(
            rectangle);
    expect(
        handles.count == 8 &&
            airshot::overlay_detail::
                hit_test_annotation_control_handle(
                    rectangle,
                    POINT{30, 30},
                    4) ==
                airshot::overlay_detail::AnnotationHandle::
                    bottom_right,
        L"selected shapes expose reliable resize handles");

    const Annotation resized =
        airshot::overlay_detail::resize_annotation_from_handle(
            rectangle,
            airshot::overlay_detail::AnnotationHandle::
                bottom_right,
            POINT{50, 40},
            100,
            100);
    expect(
        resized.start.x == 10 &&
            resized.start.y == 10 &&
            resized.end.x == 50 &&
            resized.end.y == 40,
        L"shape handles resize the selected annotation");

    const Annotation aspect_resized =
        airshot::overlay_detail::resize_annotation_from_handle(
            rectangle,
            airshot::overlay_detail::AnnotationHandle::
                bottom_right,
            POINT{50, 30},
            100,
            100,
            true);
    expect(
        aspect_resized.end.x == 50 &&
            aspect_resized.end.y == 50,
        L"Shift preserves aspect ratio while resizing annotations");

    Annotation centered_rectangle;
    centered_rectangle.tool = Tool::rectangle;
    centered_rectangle.start = {40, 40};
    centered_rectangle.end = {60, 60};
    const Annotation centered_resized =
        airshot::overlay_detail::resize_annotation_from_handle(
            centered_rectangle,
            airshot::overlay_detail::AnnotationHandle::bottom_right,
            POINT{75, 70},
            120,
            120,
            false,
            true);
    expect(
        centered_resized.start.x == 25 &&
            centered_resized.start.y == 30 &&
            centered_resized.end.x == 75 &&
            centered_resized.end.y == 70,
        L"Alt resizes an annotation handle around its stable center");

    Annotation wide_rectangle = centered_rectangle;
    wide_rectangle.start = {40, 45};
    wide_rectangle.end = {60, 55};
    const Annotation centered_aspect_resized =
        airshot::overlay_detail::resize_annotation_from_handle(
            wide_rectangle,
            airshot::overlay_detail::AnnotationHandle::bottom_right,
            POINT{80, 60},
            120,
            120,
            true,
            true);
    expect(
        centered_aspect_resized.start.x == 20 &&
            centered_aspect_resized.start.y == 35 &&
            centered_aspect_resized.end.x == 80 &&
            centered_aspect_resized.end.y == 65,
        L"Shift and Alt preserve aspect ratio while scaling from center");

    Annotation arrow = rectangle;
    arrow.tool = Tool::arrow;
    arrow.start = {30, 30};
    arrow.end = {50, 50};
    const Annotation endpoint_resized =
        airshot::overlay_detail::resize_annotation_from_handle(
            arrow,
            airshot::overlay_detail::AnnotationHandle::
                end_point,
            POINT{75, 62},
            100,
            100);
    expect(
        endpoint_resized.start.x == 30 &&
            endpoint_resized.start.y == 30 &&
            endpoint_resized.end.x == 75 &&
            endpoint_resized.end.y == 62,
        L"line and arrow endpoints can be re-edited");

    const Annotation centered_endpoint =
        airshot::overlay_detail::resize_annotation_from_handle(
            arrow,
            airshot::overlay_detail::AnnotationHandle::end_point,
            POINT{65, 60},
            100,
            100,
            false,
            true);
    expect(
        centered_endpoint.start.x + centered_endpoint.end.x ==
                arrow.start.x + arrow.end.x &&
            centered_endpoint.start.y + centered_endpoint.end.y ==
                arrow.start.y + arrow.end.y &&
            centered_endpoint.end.x == 65 &&
            centered_endpoint.end.y == 60,
        L"Alt moves the opposite line endpoint symmetrically");

    Annotation edge_line = arrow;
    edge_line.tool = Tool::line;
    edge_line.start = {0, 10};
    edge_line.end = {20, 10};
    const Annotation bounded_centered_endpoint =
        airshot::overlay_detail::resize_annotation_from_handle(
            edge_line,
            airshot::overlay_detail::AnnotationHandle::end_point,
            POINT{100, 10},
            100,
            100,
            false,
            true);
    expect(
        bounded_centered_endpoint.start.x >= 0 &&
            bounded_centered_endpoint.end.x <= 100 &&
            airshot::overlay_detail::annotation_bounds(
                bounded_centered_endpoint).left >= 0 &&
            airshot::overlay_detail::annotation_bounds(
                bounded_centered_endpoint).right <= 100,
        L"center scaling clamps both line endpoints at the canvas edge");

    expect(
        airshot::overlay_detail::keyboard_selection_step(false) == 1 &&
            airshot::overlay_detail::keyboard_selection_step(true) == 10,
        L"selection arrow keys use one pixel or ten pixels with Shift");
    const auto nudged_selection =
        airshot::overlay_detail::translate_selection_within_bounds(
            {20, 20, 50, 40},
            10,
            -10,
            {0, 0, 100, 100});
    const auto clamped_selection =
        airshot::overlay_detail::translate_selection_within_bounds(
            nudged_selection,
            100,
            -100,
            {0, 0, 100, 100});
    expect(
        nudged_selection.left == 30 && nudged_selection.top == 10 &&
            nudged_selection.width() == 30 &&
            nudged_selection.height() == 20 &&
            clamped_selection.left == 70 &&
            clamped_selection.top == 0 &&
            clamped_selection.width() == 30 &&
            clamped_selection.height() == 20,
        L"keyboard selection movement preserves size and clamps to the desktop");

    const POINT clone_offset =
        airshot::overlay_detail::preferred_clone_translation(
            rectangle,
            12,
            100,
            100);
    expect(
        clone_offset.x == 12 && clone_offset.y == 12,
        L"annotation duplication uses a visible in-bounds offset");
}

void test_repeat_region_and_selection_size_policy() {
    using airshot::DisplayMonitorGeometry;
    using airshot::LastRegionCapture;
    using airshot::RepeatRegionStatus;
    using airshot::SelectionSizeAnchor;
    using airshot::SelectionSizeParseError;

    const std::vector<DisplayMonitorGeometry> monitors{
        {{-1920, 0, 0, 1080}, false, L"LEFT"},
        {{0, 0, 1920, 1080}, true, L"PRIMARY"},
    };
    const std::vector<DisplayMonitorGeometry> reordered{
        monitors[1], monitors[0]};
    const std::wstring signature =
        airshot::display_topology_signature(monitors);
    expect(
        airshot::valid_display_topology_signature(signature) &&
            signature == airshot::display_topology_signature(reordered),
        L"display topology signature is valid and monitor-order independent");
    const auto desktop = airshot::display_topology_bounds(monitors);
    expect(
        desktop && same_rect(*desktop, {-1920, 0, 1920, 1080}),
        L"display topology bounds preserve negative monitor coordinates");

    const auto no_history = airshot::resolve_repeat_region(
        std::nullopt,
        monitors);
    expect(
        no_history.status == RepeatRegionStatus::no_history,
        L"repeat capture reports an explicit no-history state");

    const LastRegionCapture exact_history{
        {-1800, 100, -1400, 500},
        signature};
    const auto exact = airshot::resolve_repeat_region(exact_history, monitors);
    expect(
        exact && !exact.topology_changed && !exact.cropped &&
            same_rect(exact.bounds, exact_history.bounds),
        L"repeat capture keeps an exact negative-coordinate region on unchanged topology");

    const std::vector<DisplayMonitorGeometry> smaller_monitor{
        {{0, 0, 1000, 700}, true, L"PRIMARY"},
    };
    const LastRegionCapture translated_history{
        {-50, 40, 150, 240},
        L"v1-0000000000000000"};
    const auto translated = airshot::resolve_repeat_region(
        translated_history,
        smaller_monitor);
    expect(
        translated && translated.topology_changed && !translated.cropped &&
            same_rect(translated.bounds, {0, 40, 200, 240}),
        L"topology changes translate a partially visible repeat region without resizing it");

    const LastRegionCapture oversized_history{
        {-100, -50, 1100, 800},
        L"v1-0000000000000000"};
    const auto cropped = airshot::resolve_repeat_region(
        oversized_history,
        smaller_monitor);
    expect(
        cropped && cropped.cropped &&
            same_rect(cropped.bounds, {0, 0, 1000, 700}),
        L"repeat capture crops only dimensions larger than the current virtual desktop");

    const LastRegionCapture unplugged_history{
        {-1800, 100, -1400, 500},
        signature};
    const auto unplugged = airshot::resolve_repeat_region(
        unplugged_history,
        smaller_monitor);
    expect(
        unplugged.status == RepeatRegionStatus::no_intersection,
        L"repeat capture rejects a region left entirely on an unplugged monitor");

    const std::vector<DisplayMonitorGeometry> monitors_with_gap{
        {{0, 0, 100, 100}, true, L"A"},
        {{200, 0, 300, 100}, false, L"B"},
    };
    const LastRegionCapture gap_history{
        {120, 20, 180, 80},
        L"v1-0000000000000000"};
    const auto gap = airshot::resolve_repeat_region(
        gap_history,
        monitors_with_gap);
    expect(
        gap.status == RepeatRegionStatus::no_intersection,
        L"repeat capture rejects regions wholly inside a virtual-desktop monitor gap");

    const LastRegionCapture invalid_history{
        {0, 0, 1, 100},
        L"not-a-signature"};
    expect(
        airshot::resolve_repeat_region(invalid_history, monitors).status ==
            RepeatRegionStatus::invalid_history,
        L"repeat capture rejects malformed persisted history");

    const auto parsed = airshot::parse_selection_size(
        L"640",
        L"480",
        3840,
        1080);
    expect(
        parsed && parsed.width == 640 && parsed.height == 480,
        L"selection size parser accepts dimensions within virtual-desktop limits");
    expect(
        airshot::parse_selection_size(L"1", L"20", 100, 100).error ==
            SelectionSizeParseError::width_out_of_range &&
            airshot::parse_selection_size(L"20", L"101", 100, 100).error ==
                SelectionSizeParseError::height_out_of_range &&
            airshot::parse_selection_size(L"2x", L"20", 100, 100).error ==
                SelectionSizeParseError::invalid_width &&
            airshot::parse_selection_size(
                L"999999999999999999999",
                L"20",
                100,
                100)
                    .error == SelectionSizeParseError::invalid_width,
        L"selection size parser rejects range, syntax, and overflow errors");

    const airshot::RectI negative_desktop{-1920, -200, 1920, 1080};
    const auto centered = airshot::resize_selection_to_size(
        {-100, 100, 100, 300},
        400,
        300,
        negative_desktop,
        SelectionSizeAnchor::center);
    expect(
        centered && same_rect(*centered, {-200, 50, 200, 350}),
        L"center-anchored size entry preserves the selection center");
    const auto top_left = airshot::resize_selection_to_size(
        {1800, 1000, 1900, 1070},
        400,
        300,
        negative_desktop,
        SelectionSizeAnchor::top_left);
    expect(
        top_left && same_rect(*top_left, {1520, 780, 1920, 1080}),
        L"top-left size entry clamps by translation while preserving size");
    expect(
        !airshot::resize_selection_to_size(
            {0, 0, 20, 20},
            4000,
            20,
            negative_desktop,
            SelectionSizeAnchor::center),
        L"selection sizing rejects dimensions larger than the virtual desktop");

    const auto badge = airshot::selection_size_badge_bounds(
        {-1910, -195, -1800, -100},
        negative_desktop);
    expect(
        badge.left >= negative_desktop.left &&
            badge.top >= negative_desktop.top &&
            badge.right <= negative_desktop.right &&
            badge.bottom <= negative_desktop.bottom,
        L"selection size badge remains visible on negative-coordinate desktops");
    const auto tiny_badge = airshot::selection_size_badge_bounds(
        {0, 0, 2, 2},
        {0, 0, 10, 10});
    expect(
        same_rect(tiny_badge, {0, 0, 10, 10}),
        L"selection size badge safely adapts to very small synthetic desktops");
}

void test_annotation_product_styles_and_toolbar_layout() {
    using airshot::overlay_detail::Annotation;
    using airshot::overlay_detail::ArrowHeadStyle;
    using airshot::overlay_detail::ShapeFillStyle;
    using airshot::overlay_detail::StrokePattern;
    using airshot::overlay_detail::TextStyle;
    using airshot::overlay_detail::Tool;
    using airshot::overlay_detail::ToolbarButton;
    using airshot::overlay_detail::ToolbarMetrics;
    using airshot::overlay_detail::ToolStylePalette;

    ToolStylePalette styles(128);
    auto& rectangle_style = styles.for_tool(Tool::rectangle);
    rectangle_style.color = RGB(1, 2, 3);
    rectangle_style.width = 8.0F;
    rectangle_style.fill_style = ShapeFillStyle::translucent;
    rectangle_style.stroke_pattern = StrokePattern::dashed;
    rectangle_style.rounded_rectangle = true;
    const auto& pen_style = styles.for_tool(Tool::pen);
    const auto& highlight_style = styles.for_tool(Tool::highlight);
    expect(
        rectangle_style.color == RGB(1, 2, 3) &&
            rectangle_style.width == 8.0F &&
            rectangle_style.fill_style == ShapeFillStyle::translucent &&
            rectangle_style.stroke_pattern == StrokePattern::dashed &&
            rectangle_style.rounded_rectangle &&
            pen_style.color == RGB(245, 34, 45) &&
            pen_style.width == 4.0F &&
            highlight_style.color == RGB(250, 219, 20) &&
            highlight_style.highlight_alpha == 128,
        L"each annotation tool keeps an independent style within one capture");

    const auto horizontal = airshot::overlay_detail::orthogonal_endpoint(
        POINT{10, 10},
        POINT{40, 19});
    const auto vertical = airshot::overlay_detail::orthogonal_endpoint(
        POINT{10, 10},
        POINT{14, 50});
    expect(
        horizontal.x == 40 && horizontal.y == 10 &&
            vertical.x == 10 && vertical.y == 50,
        L"Shift highlight constraint selects the dominant axis");

    expect(
        airshot::overlay_detail::arrow_head_length(0.0F) == 10.0F &&
            airshot::overlay_detail::arrow_head_length(100.0F) == 34.0F &&
            airshot::overlay_detail::arrow_has_start_head(
                ArrowHeadStyle::reverse) &&
            airshot::overlay_detail::arrow_has_start_head(
                ArrowHeadStyle::both) &&
            airshot::overlay_detail::arrow_has_end_head(
                ArrowHeadStyle::forward) &&
            airshot::overlay_detail::arrow_has_end_head(
                ArrowHeadStyle::both),
        L"arrow heads expose bounded width-aware forward/reverse/both modes");
    const auto short_wings = airshot::overlay_detail::arrow_head_wings(
        POINT{5, 0},
        POINT{0, 0},
        50.0F);
    expect(
        short_wings.first.x >= 0 && short_wings.second.x >= 0 &&
            short_wings.first.x <= 5 && short_wings.second.x <= 5,
        L"arrow heads remain bounded by short shaft geometry");

    std::vector<POINT> bounded_stroke;
    const bool first_added =
        airshot::overlay_detail::append_stroke_point_bounded(
            bounded_stroke, POINT{0, 0}, 2.0, 2);
    const bool jitter_added =
        airshot::overlay_detail::append_stroke_point_bounded(
            bounded_stroke, POINT{1, 0}, 2.0, 2);
    const bool second_added =
        airshot::overlay_detail::append_stroke_point_bounded(
            bounded_stroke, POINT{2, 0}, 2.0, 2);
    const bool over_limit_added =
        airshot::overlay_detail::append_stroke_point_bounded(
            bounded_stroke, POINT{4, 0}, 2.0, 2);
    expect(
        first_added && !jitter_added && second_added &&
            !over_limit_added && bounded_stroke.size() == 2,
        L"pen input removes pointer jitter and enforces a hard point bound");
    const std::vector<POINT> noisy_stroke{
        {0, 0}, {2, 6}, {4, 0}, {6, 6}, {8, 0}};
    const auto smoothed =
        airshot::overlay_detail::smooth_polyline(noisy_stroke, 2, 32);
    expect(
        !smoothed.empty() && smoothed.front().x == 0 &&
            smoothed.front().y == 0 && smoothed.back().x == 8 &&
            smoothed.back().y == 0 && smoothed.size() <= noisy_stroke.size(),
        L"pen smoothing is bounded and preserves both endpoints");
    const std::vector<POINT> click_dot{{7, 9}};
    const auto smoothed_click =
        airshot::overlay_detail::smooth_polyline(click_dot);
    expect(
        smoothed_click.size() == 1 && smoothed_click.front().x == 7 &&
            smoothed_click.front().y == 9,
        L"pen smoothing preserves a single-click dot");

    Annotation serial;
    serial.tool = Tool::serial;
    serial.start = {50, 50};
    serial.end = serial.start;
    serial.width = 4.0F;
    serial.serial = 100;
    const float two_digit_radius =
        airshot::overlay_detail::serial_visual_radius(4.0F, 99);
    const float three_digit_radius =
        airshot::overlay_detail::serial_visual_radius(4.0F, 100);
    const auto serial_bounds =
        airshot::overlay_detail::annotation_bounds(serial);
    expect(
        airshot::overlay_detail::serial_digit_count(100) == 3 &&
            three_digit_radius > two_digit_radius &&
            airshot::overlay_detail::serial_font_size(4.0F, 100) <
                three_digit_radius &&
            serial_bounds.width() >=
                static_cast<int>(std::ceil(three_digit_radius * 2.0F)),
        L"three-digit serial markers adapt circle, font, and hit bounds");

    Annotation zero_effect;
    zero_effect.tool = Tool::blur;
    zero_effect.points = {{10, 10}};
    zero_effect.alpha = 0;
    expect(
        !airshot::overlay_detail::annotation_is_committable(zero_effect),
        L"zero-strength mosaic and blur gestures are not committed");

    Annotation hollow_rectangle;
    hollow_rectangle.tool = Tool::rectangle;
    hollow_rectangle.start = {10, 10};
    hollow_rectangle.end = {110, 90};
    hollow_rectangle.width = 4.0F;
    expect(
        airshot::overlay_detail::shape_annotation_hit_test(
            hollow_rectangle, POINT{10, 50}) &&
            !airshot::overlay_detail::shape_annotation_hit_test(
                hollow_rectangle, POINT{60, 50}) &&
            airshot::overlay_detail::shape_annotation_hit_test(
                hollow_rectangle, POINT{3, 50}) &&
            !airshot::overlay_detail::shape_annotation_hit_test(
                hollow_rectangle, POINT{0, 50}),
        L"hollow rectangle hit testing follows its width-aware stroke instead of its interior bounds");
    Annotation filled_rectangle = hollow_rectangle;
    filled_rectangle.fill_style = ShapeFillStyle::translucent;
    expect(
        airshot::overlay_detail::shape_annotation_hit_test(
            filled_rectangle, POINT{60, 50}),
        L"filled rectangle hit testing includes the interior");

    Annotation hollow_ellipse = hollow_rectangle;
    hollow_ellipse.tool = Tool::ellipse;
    hollow_ellipse.start = {150, 10};
    hollow_ellipse.end = {250, 90};
    expect(
        airshot::overlay_detail::shape_annotation_hit_test(
            hollow_ellipse, POINT{200, 10}) &&
            !airshot::overlay_detail::shape_annotation_hit_test(
                hollow_ellipse, POINT{200, 50}),
        L"hollow ellipse hit testing follows the curved stroke and excludes its center");
    hollow_ellipse.fill_style = ShapeFillStyle::translucent;
    expect(
        airshot::overlay_detail::shape_annotation_hit_test(
            hollow_ellipse, POINT{200, 50}),
        L"filled ellipse hit testing includes the interior");

    Annotation rounded_rectangle = hollow_rectangle;
    rounded_rectangle.width = 2.0F;
    rounded_rectangle.rounded_rectangle = true;
    expect(
        !airshot::overlay_detail::shape_annotation_hit_test(
            rounded_rectangle, POINT{10, 10}, 0.0) &&
            airshot::overlay_detail::shape_annotation_hit_test(
                rounded_rectangle, POINT{12, 12}, 0.0),
        L"rounded rectangle hit testing excludes the clipped corner and includes its arc");

    expect(
        airshot::overlay_detail::tool_uses_bitmap_effect_preview(
            Tool::highlight) &&
            airshot::overlay_detail::tool_uses_bitmap_effect_preview(
                Tool::mosaic) &&
            !airshot::overlay_detail::tool_uses_bitmap_effect_preview(
                Tool::pen),
        L"highlight preview uses the same bitmap effect pipeline as its committed render");
    expect(
        airshot::overlay_detail::should_persist_tool_style(
            Tool::rectangle, true) &&
            airshot::overlay_detail::should_persist_tool_style(
                Tool::select, false) &&
            !airshot::overlay_detail::should_persist_tool_style(
                Tool::select, true),
        L"selecting and editing a history object cannot overwrite new-tool defaults");
    expect(
        airshot::overlay_detail::effect_geometry_mode_available(
            Tool::mosaic, Tool::mosaic, false) &&
            !airshot::overlay_detail::effect_geometry_mode_available(
                Tool::select, Tool::mosaic, true) &&
            !airshot::overlay_detail::effect_geometry_mode_available(
                Tool::select, Tool::blur, true),
        L"existing mosaic and blur objects do not expose a silent no-op geometry mode switch");

    Annotation styled_history;
    styled_history.tool = Tool::rectangle;
    styled_history.start = {10, 10};
    styled_history.end = {30, 30};
    styled_history.fill_style = ShapeFillStyle::translucent;
    styled_history.stroke_pattern = StrokePattern::dashed;
    styled_history.rounded_rectangle = true;
    std::vector<Annotation> document{styled_history};
    airshot::overlay_detail::AnnotationHistory history;
    history.record(document);
    document.front().fill_style = ShapeFillStyle::outline;
    document.front().stroke_pattern = StrokePattern::solid;
    document.front().rounded_rectangle = false;
    expect(
        history.undo(document) &&
            document.front().fill_style == ShapeFillStyle::translucent &&
            document.front().stroke_pattern == StrokePattern::dashed &&
            document.front().rounded_rectangle,
        L"undo restores all product shape styles atomically");

    Annotation normal_text;
    normal_text.tool = Tool::text;
    normal_text.start = {20, 20};
    normal_text.end = normal_text.start;
    normal_text.text = L"Air";
    normal_text.width = 18.0F;
    Annotation dark_text = normal_text;
    dark_text.text_style = TextStyle::dark;
    Annotation outline_text = normal_text;
    outline_text.text_style = TextStyle::outline;
    const auto normal_bounds =
        airshot::overlay_detail::annotation_bounds(normal_text);
    const auto dark_bounds =
        airshot::overlay_detail::annotation_bounds(dark_text);
    const auto outline_bounds =
        airshot::overlay_detail::annotation_bounds(outline_text);
    expect(
        dark_bounds.left == normal_bounds.left &&
            dark_bounds.top == normal_bounds.top &&
            dark_bounds.right == normal_bounds.right + 8 &&
            dark_bounds.bottom == normal_bounds.bottom + 6 &&
            outline_bounds.left == normal_bounds.left - 2 &&
            outline_bounds.top == normal_bounds.top - 2,
        L"normal, dark, and outline text bounds match their rendered footprint");

    const std::vector<std::pair<std::wstring, std::wstring>> rectangle_items{
        {L"width_small", L""}, {L"width_medium", L""},
        {L"width_large", L""}, {L"|", L""},
        {L"color_red", L""}, {L"color_green", L""},
        {L"color_blue", L""}, {L"color_yellow", L""},
        {L"color_black", L""}, {L"color_gray", L""},
        {L"color_white", L""}, {L"color_custom", L""},
        {L"|", L""}, {L"fill_outline", L"空心"},
        {L"fill_translucent", L"填充"}, {L"|", L""},
        {L"corner_square", L"直角"}, {L"corner_round", L"圆角"},
        {L"|", L""}, {L"stroke_solid", L"实线"},
        {L"stroke_dashed", L"虚线"},
    };
    const auto verify_toolbar = [&](airshot::RectI monitor,
                                    ToolbarMetrics metrics,
                                    std::wstring_view message) {
        const auto rows = airshot::overlay_detail::wrap_toolbar_items(
            rectangle_items,
            metrics,
            monitor);
        const int width =
            airshot::overlay_detail::toolbar_width(rows, metrics);
        const int height =
            airshot::overlay_detail::toolbar_height(rows, metrics);
        const int left = airshot::overlay_detail::clamp_toolbar_axis(
            monitor.right - width,
            width,
            monitor.left,
            monitor.right);
        const int top = airshot::overlay_detail::clamp_toolbar_axis(
            monitor.bottom - height,
            height,
            monitor.top,
            monitor.bottom);
        std::vector<ToolbarButton> buttons;
        airshot::overlay_detail::place_toolbar_rows(
            buttons, rows, metrics, left, top);
        const bool all_inside = std::ranges::all_of(
            buttons,
            [&](const ToolbarButton& button) {
                return button.bounds.left >= monitor.left &&
                       button.bounds.top >= monitor.top &&
                       button.bounds.right <= monitor.right &&
                       button.bounds.bottom <= monitor.bottom;
            });
        expect(
            !rows.empty() && !buttons.empty() && width <= monitor.width() &&
                height <= monitor.height() && all_inside,
            message);
    };
    verify_toolbar(
        {0, 0, 1280, 720},
        {36, 34, 4, 8},
        L"expanded sub-toolbar fits a 1280 by 720 monitor");
    verify_toolbar(
        {-1280, -120, 0, 600},
        {36, 34, 4, 8},
        L"expanded sub-toolbar fits a negative-coordinate monitor");
    verify_toolbar(
        {0, 0, 1280, 720},
        {54, 51, 6, 12},
        L"expanded sub-toolbar remains bounded with 150 percent metrics");
}

void test_config() {
    expect(airshot::AppConfig{}.ocr_engine == airshot::kDefaultOcrEngine &&
               airshot::kDefaultOcrEngine == airshot::kOcrEngineRapidV5Fast,
           L"default OCR engine is local high accuracy");
    expect(
        airshot::AppConfig{}.toolbar_order.starts_with(L"lock,"),
        L"new installations expose continuous annotation in the toolbar");

    airshot::AppConfig config;
    config.annotation_enabled = false;
    config.annotation_locked_tool = false;
    config.annotation_hidden_tools = L"pen,banana,rect,pen,close";
    config.annotation_highlight_alpha = 300;
    config.global_ocr_enabled = true;
    config.capture_hotkey = L"Ctrl+Shift+F9";
    config.global_ocr_hotkey = L"Ctrl+Alt+O";
    config.capture_cursor = true;
    config.custom_color = L"#123456";
    config.tool_shortcut_select = L"Shift+S";
    config.tool_shortcut_rectangle = L"Shift+R";
    config.toolbar_order = L"rect,pen,close";
    config.text_font_family = L"Consolas";
    config.text_font_bold = true;
    config.text_font_italic = true;
    config.app_icon = std::wstring(airshot::kAppIconFlowLens);
    config.ocr_engine = std::wstring(airshot::kOcrEngineRapidV5Accurate);
    config.ocr_download_url = L"https://example.com/ocr-dependencies.json";
    const auto parsed = airshot::config_from_json(airshot::config_to_json(config));
    expect(parsed.has_value(), L"config JSON round trip parses");
    expect(parsed && !parsed->annotation_enabled && parsed->global_ocr_enabled &&
               !parsed->annotation_locked_tool && parsed->annotation_hidden_tools == L"rect,pen,close" &&
               parsed->annotation_highlight_alpha == 192 &&
               parsed->capture_hotkey == L"Ctrl+Shift+F9" && parsed->global_ocr_hotkey == L"Ctrl+Alt+O" &&
               parsed->capture_cursor &&
               parsed->custom_color == L"#123456" &&
               parsed->tool_shortcut_select == L"Shift+S" &&
               parsed->tool_shortcut_rectangle == L"Shift+R" &&
               parsed->toolbar_order == airshot::kDefaultToolbarOrder &&
               parsed->text_font_family == L"Consolas" &&
               parsed->text_font_bold == true &&
               parsed->text_font_italic == true &&
               parsed->app_icon == airshot::kAppIconFlowLens &&
               parsed->ocr_engine == airshot::kOcrEngineRapidV5Accurate &&
               parsed->ocr_download_url == L"https://example.com/ocr-dependencies.json",
           L"config JSON round trip values");

    const auto future = airshot::config_from_json(
        LR"({"schemaVersion":2,"annotation":{"enabled":false},"future":[null,true,{"name":"\u4E2D"}]})");
    expect(future && future->schema_version == 2 && !future->annotation_enabled &&
               future->annotation_locked_tool && future->annotation_hidden_tools.empty() &&
               future->toolbar_order.starts_with(L"lock,") &&
               !future->capture_cursor &&
               future->annotation_highlight_alpha == 96 &&
               future->text_font_family == L"Microsoft YaHei" && !future->text_font_bold && !future->text_font_italic &&
               future->app_icon == airshot::kDefaultAppIcon &&
               future->ocr_engine == airshot::kDefaultOcrEngine,
           L"config accepts unknown future fields and keeps annotation defaults");
    constexpr std::wstring_view normalized_legacy_toolbar_order =
        L"lock,select,rect,ellipse,line,arrow,pen,mosaic,blur,highlight,watermark,text,serial,eraser,undo,redo,ocr,scroll,pin,save,close,copy";
    const auto explicit_legacy_toolbar = airshot::config_from_json(
        LR"({"schemaVersion":2,"annotation":{"toolbarOrder":"lock,select,rect,ellipse,line,arrow,pen,mosaic,blur,highlight,text,serial,eraser,undo,redo,ocr,scroll,pin,save,copy"}})");
    expect(
        explicit_legacy_toolbar &&
            explicit_legacy_toolbar->toolbar_order ==
                normalized_legacy_toolbar_order,
        L"legacy toolbar order keeps known relative order and gains missing tools");
    const std::wstring only_blur =
        airshot::normalize_toolbar_order(L"blur");
    const std::wstring only_mosaic =
        airshot::normalize_toolbar_order(L"mosaic");
    expect(
        only_blur == airshot::kDefaultToolbarOrder &&
            only_mosaic == airshot::kDefaultToolbarOrder &&
            only_blur.find(L"mosaic,blur") != std::wstring::npos,
        L"single legacy blur or mosaic layouts regain the unified effect anchors");
    const std::wstring customized_toolbar =
        airshot::normalize_toolbar_order(
            L"copy,unknown,RECT,copy,lock");
    expect(
        customized_toolbar.find(L"unknown") == std::wstring::npos &&
            customized_toolbar.find(L"copy") <
                customized_toolbar.find(L"rect") &&
            customized_toolbar.find(L"rect") <
                customized_toolbar.find(L"lock") &&
            customized_toolbar.find(L"copy", 1) == std::wstring::npos,
        L"toolbar normalization filters unknowns, deduplicates, and preserves custom order");
    const auto legacy_serial = airshot::config_from_json(
        LR"({"annotation":{"nextSerial":37}})");
    expect(
        legacy_serial.has_value(),
        L"legacy persisted serial counters remain readable");
    expect(
        legacy_serial &&
            airshot::config_to_json(*legacy_serial).find(L"nextSerial") ==
                std::wstring::npos,
        L"legacy persisted serial counters are removed on save");
    expect(!airshot::config_from_json(L"{\"annotation\":[}"), L"config rejects malformed JSON");
    const auto old_numeric_ocr = airshot::config_from_json(LR"({"ocr":{"engine":1}})");
    expect(old_numeric_ocr && old_numeric_ocr->ocr_engine == airshot::kDefaultOcrEngine,
           L"config migrates old numeric OCR engine");
    const auto invalid_ocr = airshot::config_from_json(LR"({"ocr":{"engine":"banana"}})");
    expect(invalid_ocr && invalid_ocr->ocr_engine == airshot::kDefaultOcrEngine,
           L"config clamps invalid OCR engine");
    expect(airshot::normalize_ocr_engine(L"wechat") == airshot::kOcrEngineRapidV5Fast,
           L"config migrates legacy WeChat OCR engine");
    expect(airshot::normalize_app_icon(L"pixel-console") == airshot::kAppIconPixelConsole &&
               airshot::normalize_app_icon(L"banana") == airshot::kDefaultAppIcon,
           L"config normalizes supported and unknown application icons");
    expect(!airshot::config_from_json(
               LR"({"schemaVersion":2,"shell":{"appIcon":"banana"}})"),
           L"current config rejects an unsupported application icon");

    expect(airshot::normalize_annotation_hidden_tools(L"pen,unknown,RECT;pen close") == L"rect,pen,close",
           L"hidden annotation tools normalize, dedupe, and skip unknown values");
    expect(airshot::annotation_tool_hidden(L"rect,pen", L"PEN") &&
               !airshot::annotation_tool_hidden(L"rect,pen", L"arrow"),
           L"hidden annotation tool lookup");

    const auto hotkey = airshot::parse_hotkey(L"Ctrl+Alt+A");
    expect(hotkey && (hotkey->modifiers & MOD_CONTROL) && (hotkey->modifiers & MOD_ALT) &&
               hotkey->virtual_key == L'A',
           L"hotkey parser");
    expect(!airshot::parse_hotkey(L"Ctrl+Banana"), L"hotkey parser rejects invalid values");
}

void test_cli() {
    const std::vector<std::wstring> arguments{
        L"capture", L"screen", L"--monitor", L"primary", L"--output", L"file", L"--path", L"C:\\shot.png", L"--json"};
    const auto parsed = airshot::parse_cli(arguments);
    expect(parsed.code == airshot::ExitCode::success && parsed.json && !parsed.request_json.empty(),
           L"CLI screen command");

    const std::vector<std::wstring> invalid{L"capture", L"banana"};
    expect(airshot::parse_cli(invalid).code == airshot::ExitCode::invalid_arguments, L"CLI rejects invalid mode");

    airshot::CommandResponse response;
    response.code = airshot::ExitCode::success;
    response.message = L"ok";
    response.path = L"C:\\shot.png";
    const auto round_trip = airshot::response_from_json(airshot::response_to_json(response));
    expect(round_trip.code == airshot::ExitCode::success && round_trip.message == L"ok" &&
               round_trip.path == L"C:\\shot.png",
           L"response JSON round trip");
}

void test_ocr_join() {
    const std::vector<std::wstring> lines{L"第一行", L"second"};
    expect(airshot::join_ocr_lines(lines) == L"第一行\r\nsecond", L"OCR line join");
}

void test_ocr_dependency_manifest() {
    const auto manifest = airshot::parse_ocr_dependency_manifest(
        LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[)"
        LR"({"path":"rapidocr_api.dll","url":"https://example.com/rapidocr_api.dll","sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","size":1},)"
        LR"({"path":"onnxruntime.dll","url":"https://example.com/onnxruntime.dll","sha256":"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB","size":2},)"
        LR"({"path":"rapidocr_runner.exe","url":"https://example.com/rapidocr_runner.exe","sha256":"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC","size":3},)"
        LR"({"path":"models/rapidocr-v5-fast/det.onnx","url":"https://example.com/v5-fast-det.onnx","sha256":"DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD","size":4},)"
        LR"({"path":"models/rapidocr-v5-fast/rec.onnx","url":"https://example.com/v5-fast-rec.onnx","sha256":"EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE","size":5},)"
        LR"({"path":"models/rapidocr-v5-fast/cls.onnx","url":"https://example.com/v5-fast-cls.onnx","sha256":"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF","size":6},)"
        LR"({"path":"models/rapidocr-v5-fast/dict.txt","url":"https://example.com/v5-fast-dict.txt","sha256":"1111111111111111111111111111111111111111111111111111111111111111","size":7},)"
        LR"({"path":"models/rapidocr-v5-accurate/det.onnx","url":"https://example.com/v5-accurate-det.onnx","sha256":"2222222222222222222222222222222222222222222222222222222222222222","size":8},)"
        LR"({"path":"models/rapidocr-v5-accurate/rec.onnx","url":"https://example.com/v5-accurate-rec.onnx","sha256":"3333333333333333333333333333333333333333333333333333333333333333","size":9},)"
        LR"({"path":"models/rapidocr-v5-accurate/cls.onnx","url":"https://example.com/v5-accurate-cls.onnx","sha256":"4444444444444444444444444444444444444444444444444444444444444444","size":10},)"
        LR"({"path":"models/rapidocr-v5-accurate/dict.txt","url":"https://example.com/v5-accurate-dict.txt","sha256":"5555555555555555555555555555555555555555555555555555555555555555","size":11},)"
        LR"({"path":"models/rapidocr-v4-compat/det.onnx","url":"https://example.com/v4-compat-det.onnx","sha256":"6666666666666666666666666666666666666666666666666666666666666666","size":12},)"
        LR"({"path":"models/rapidocr-v4-compat/rec.onnx","url":"https://example.com/v4-compat-rec.onnx","sha256":"7777777777777777777777777777777777777777777777777777777777777777","size":13},)"
        LR"({"path":"models/rapidocr-v4-compat/cls.onnx","url":"https://example.com/v4-compat-cls.onnx","sha256":"8888888888888888888888888888888888888888888888888888888888888888","size":14},)"
        LR"({"path":"models/rapidocr-v4-compat/dict.txt","url":"https://example.com/v4-compat-dict.txt","sha256":"9999999999999999999999999999999999999999999999999999999999999999","size":15})"
        LR"(]})");
    expect(manifest && manifest->package_id == airshot::kRapidOcrOnnxPackageId && manifest->files.size() == 15,
           L"OCR dependency manifest parses required files");

    expect(!airshot::parse_ocr_dependency_manifest(
               LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[{"path":"../rapidocr_api.dll","url":"https://example.com/a","sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","size":1}]})"),
           L"OCR dependency manifest rejects unsafe paths");
    expect(!airshot::parse_ocr_dependency_manifest(
               LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[{"path":"rapidocr_api.dll","url":"http://example.com/a","sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","size":1}]})"),
           L"OCR dependency manifest rejects non-HTTPS URLs");
    expect(!airshot::parse_ocr_dependency_manifest(
               LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[{"path":"rapidocr_api.dll","url":"https://example.com/a","sha256":"0000000000000000000000000000000000000000000000000000000000000000","size":1}]})"),
           L"OCR dependency manifest rejects zero SHA256");
    expect(!airshot::parse_ocr_dependency_manifest(
               LR"({"schemaVersion":1,"packageId":"rapidocr-onnx","sequence":1000000,"issuedAt":1800000000,"expiresAt":1810000000,"files":[{"path":"rapidocr_api.dll","url":"https://example.com/a","sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","size":0}]})"),
           L"OCR dependency manifest rejects zero size");
}

void test_portable_runtime() {
    expect(airshot::version_is_newer(L"0.1.6", L"0.2.0"), L"portable version detects newer release");
    expect(!airshot::version_is_newer(L"0.2.0", L"0.1.6"), L"portable version rejects older release");
    expect(!airshot::version_is_newer(L"0.2", L"0.2.0"), L"portable version rejects malformed version");

    const auto manifest = airshot::parse_update_manifest(
        LR"({"version":"0.2.0","url":"https://example.com/AirScreenshot.exe","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","size":3})");
    expect(manifest && manifest->version == L"0.2.0" && manifest->size == 3 &&
               manifest->sha256 == L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
           L"portable update manifest parses and normalizes");
    expect(!airshot::parse_update_manifest(
               LR"({"version":"0.2","url":"http://example.com/a.exe","sha256":"bad","size":0})"),
           L"portable update manifest rejects unsafe values");

    const auto path = std::filesystem::temp_directory_path() / L"airshot-sha256-test.txt";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "abc";
    }
    expect(airshot::sha256_file(path) == L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
           L"portable SHA256");
    airshot::UpdateManifest unsigned_manifest{
        L"0.2.0",
        L"https://example.com/AirScreenshot.exe",
        L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
        3};
    expect(!airshot::verify_portable_executable(path, unsigned_manifest),
           L"portable update rejects unsigned executable");
    unsigned_manifest.sha256 = L"AA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
    expect(!airshot::verify_portable_executable(path, unsigned_manifest),
           L"portable update rejects wrong hash");
    unsigned_manifest.sha256 = L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
    unsigned_manifest.size = 4;
    expect(!airshot::verify_portable_executable(path, unsigned_manifest),
           L"portable update rejects wrong size");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    wchar_t windows_directory[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows_directory, MAX_PATH) > 0) {
        const auto wrong_signer = std::filesystem::path(windows_directory) / L"System32" / L"notepad.exe";
        if (std::filesystem::exists(wrong_signer)) {
            airshot::UpdateManifest wrong_signer_manifest{
                L"0.2.0",
                L"https://example.com/AirScreenshot.exe",
                airshot::sha256_file(wrong_signer),
                std::filesystem::file_size(wrong_signer)};
            expect(!airshot::verify_portable_executable(wrong_signer, wrong_signer_manifest),
                   L"portable update rejects wrong signer");
        }
    }

    expect(airshot::portable_startup_command(L"C:\\Apps\\Air Screenshot\\AirScreenshot.exe") ==
               L"\"C:\\Apps\\Air Screenshot\\AirScreenshot.exe\"",
           L"portable startup command quotes executable path");
}

}  // namespace

void test_overlay_ui_dpi_metrics() {
    using airshot::overlay_detail::OverlayUiMetrics;
    using airshot::overlay_detail::ToolbarMetrics;
    using airshot::overlay_detail::scale_overlay_ui_px;
    using airshot::overlay_detail::toolbar_item_width;

    expect(scale_overlay_ui_px(9, 96) == 9 &&
               scale_overlay_ui_px(9, 144) == 14 &&
               scale_overlay_ui_px(9, 192) == 18,
           L"overlay UI integer scaling rounds consistently at 96/144/192 DPI");
    expect(scale_overlay_ui_px(40, 96) == 40 &&
               scale_overlay_ui_px(40, 144) == 60 &&
               scale_overlay_ui_px(40, 192) == 80,
           L"overlay UI button scaling at 96/144/192 DPI");

    const auto verify_toolbar = [&](unsigned int dpi,
                                    int button,
                                    int drag,
                                    int separator,
                                    int slider) {
        const OverlayUiMetrics ui{dpi};
        const ToolbarMetrics metrics{
            ui.px(40), ui.px(40), ui.px(4), ui.px(8), dpi};
        expect(metrics.button_width == button &&
                   toolbar_item_width(L"drag", metrics) == drag &&
                   toolbar_item_width(L"|", metrics) == separator &&
                   toolbar_item_width(L"mosaic_strength_slider", metrics) == slider,
               L"toolbar layout and special items share one DPI scale");
    };
    verify_toolbar(96, 40, 20, 9, 188);
    verify_toolbar(144, 60, 30, 14, 282);
    verify_toolbar(192, 80, 40, 18, 376);

    const airshot::RectI selection{100, 200, 500, 600};
    const airshot::RectI desktop{0, 0, 2000, 1200};
    expect(same_rect(
               airshot::selection_size_badge_bounds(selection, desktop, 96),
               {100, 172, 260, 196}),
           L"selection badge metrics at 96 DPI");
    expect(same_rect(
               airshot::selection_size_badge_bounds(selection, desktop, 144),
               {100, 158, 340, 194}),
           L"selection badge metrics at 144 DPI");
    expect(same_rect(
               airshot::selection_size_badge_bounds(selection, desktop, 192),
               {100, 144, 420, 192}),
           L"selection badge metrics at 192 DPI");
    expect(selection.width() == 400 && selection.height() == 400,
           L"DPI scaling leaves screenshot canvas coordinates unchanged");
}

int wmain() {
    test_overlay_ui_dpi_metrics();
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    test_rect_and_bitmap();
    test_window_candidate_hierarchy();
    test_pin_recovery_layout();
    test_repeat_region_and_selection_size_policy();
    test_serial_counter_is_scoped_to_capture_session();
    test_annotation_geometry_and_history();
    test_annotation_product_styles_and_toolbar_layout();
    test_config();
    test_cli();
    test_ocr_join();
    test_ocr_dependency_manifest();
    test_portable_runtime();
    if (failures == 0) {
        std::wcout << L"All tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
