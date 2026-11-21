#include "faithfulframe-preview-geometry.hpp"
#include "faithfulframe-preview-render-stamp.hpp"
#include "faithfulframe-preview-selection.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>

static void check_impl(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "preview geometry check failed at line %d\n", line);
		std::abort();
	}
}

#define check(condition) check_impl((condition), __LINE__)

static bool near(double actual, double expected)
{
	return std::abs(actual - expected) < 1e-6;
}

int main()
{
	faithfulframe_preview_candidate_geometry prior_candidates[2]{};
	prior_candidates[0] = {10, {0.2f, 0.3f, 0.1f, 0.2f}};
	prior_candidates[1] = {11, {0.7f, 0.3f, 0.1f, 0.2f}};
	faithfulframe_preview_candidate_geometry current_candidates[2]{};
	current_candidates[0] = prior_candidates[0];
	current_candidates[1] = prior_candidates[1];
	check(faithfulframe_preview_candidate_geometry_matches(prior_candidates, 2, current_candidates, 2));
	current_candidates[0].rect.x += 0.01f;
	check(!faithfulframe_preview_candidate_geometry_matches(prior_candidates, 2, current_candidates, 2));
	current_candidates[0] = prior_candidates[0];
	current_candidates[0].handle++;
	check(!faithfulframe_preview_candidate_geometry_matches(prior_candidates, 2, current_candidates, 2));
	current_candidates[0] = prior_candidates[1];
	current_candidates[1] = prior_candidates[0];
	check(!faithfulframe_preview_candidate_geometry_matches(prior_candidates, 2, current_candidates, 2));
	check(!faithfulframe_preview_candidate_geometry_matches(prior_candidates, 2, current_candidates, 1));
	check(!faithfulframe_preview_candidate_geometry_matches(nullptr, 1, current_candidates, 1));
	check(faithfulframe_preview_candidate_geometry_matches(nullptr, 0, nullptr, 0));

	faithfulframe_preview_render_stamp rendered_stamp{};
	rendered_stamp.valid = true;
	rendered_stamp.source_id = 1;
	rendered_stamp.session_id = 2;
	rendered_stamp.status_version = 3;
	rendered_stamp.observation_version = 4;
	rendered_stamp.source_generation = 5;
	rendered_stamp.session_generation = 6;
	rendered_stamp.stream_generation = 7;
	rendered_stamp.geometry_generation = 8;
	rendered_stamp.tracker_generation = 9;
	rendered_stamp.candidate_preview_revision = 10;
	rendered_stamp.display_width = 1920;
	rendered_stamp.display_height = 1080;
	rendered_stamp.output_width = 1280;
	rendered_stamp.output_height = 720;
	rendered_stamp.crop = {0.25f, 0.25f, 0.5f, 0.5f};
	rendered_stamp.viewport = {0.0f, 0.125f, 1.0f, 0.75f};
	faithfulframe_preview_render_stamp current_stamp = rendered_stamp;
	check(faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp.valid = false;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.observation_version++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.status_version++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.source_generation++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.session_generation++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.stream_generation++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.geometry_generation++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.tracker_generation++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.candidate_preview_revision++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.source_id++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.session_id++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.display_width++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.display_height++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.full_source = true;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.crop.x += 0.01f;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.viewport.height -= 0.01f;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	current_stamp.output_width++;
	check(!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp));
	current_stamp = rendered_stamp;
	ff_rect click_candidates[] = {
		{0.0f, 0.0f, 0.1f, 0.1f}, // Outside the current source crop.
		{0.5f, 0.375f, 0.25f, 0.25f},
	};
	std::size_t selected_candidate = 73;
	check(ff_preview_select_candidate(rendered_stamp, current_stamp, 128.0, 72.0, 256.0, 144.0, click_candidates, 2,
					  selected_candidate) == ff_preview_click_result::selected);
	check(selected_candidate == 1); // Returned index addresses the original candidate array.
	selected_candidate = 73;
	check(ff_preview_select_candidate(rendered_stamp, current_stamp, 256.0, 144.0, 512.0, 288.0, click_candidates,
					  2, selected_candidate) == ff_preview_click_result::selected);
	check(selected_candidate == 1); // Logical/HiDPI sizes preserve the click location.
	check(ff_preview_select_candidate(rendered_stamp, current_stamp, 4.0, 72.0, 400.0, 144.0, click_candidates, 2,
					  selected_candidate) == ff_preview_click_result::outside_image);
	check(selected_candidate == 1); // Rejections never publish an index.
	faithfulframe_preview_render_stamp stale_click_stamp = current_stamp;
	stale_click_stamp.status_version++;
	check(ff_preview_select_candidate(rendered_stamp, stale_click_stamp, 128.0, 72.0, 256.0, 144.0,
					  click_candidates, 2,
					  selected_candidate) == ff_preview_click_result::stale_preview);
	check(selected_candidate == 1);
	ff_rect overlapping_click_candidates[] = {
		{0.5f, 0.375f, 0.25f, 0.25f},
		{0.5f, 0.375f, 0.25f, 0.25f},
	};
	check(ff_preview_select_candidate(rendered_stamp, current_stamp, 128.0, 72.0, 256.0, 144.0,
					  overlapping_click_candidates, 2,
					  selected_candidate) == ff_preview_click_result::ambiguous);
	check(selected_candidate == 1);
	ff_rect invalid_click_candidates[] = {
		{NAN, 0.375f, 0.25f, 0.25f},
		{0.5f, 0.375f, 0.25f, 0.25f},
	};
	check(ff_preview_select_candidate(rendered_stamp, current_stamp, 128.0, 72.0, 256.0, 144.0,
					  invalid_click_candidates, 2,
					  selected_candidate) == ff_preview_click_result::invalid_geometry);
	check(selected_candidate == 1);
	std::array<ff_rect, faithfulframe_max_candidates + 1> excess_click_candidates{};
	check(ff_preview_select_candidate(rendered_stamp, current_stamp, 128.0, 72.0, 256.0, 144.0,
					  excess_click_candidates.data(), excess_click_candidates.size(),
					  selected_candidate) == ff_preview_click_result::too_many_candidates);
	check(selected_candidate == 1);
	check(!faithfulframe_preview_render_stamp_matches(faithfulframe_preview_render_stamp{}, rendered_stamp));

	check(ff_preview_observation_is_fresh(1000, 1500, 500));
	check(!ff_preview_observation_is_fresh(1000, 1501, 500));
	check(!ff_preview_observation_is_fresh(1501, 1500, 500));
	check(!ff_preview_observation_is_fresh(0, 0, 500));
	check(ff_preview_source_geometry_matches(3840, 2160, 3840, 2160));
	check(!ff_preview_source_geometry_matches(3840, 2160, 2160,
						  3840)); // Rotation or resize awaits a new session tick.
	check(!ff_preview_source_geometry_matches(0, 2160, 0, 2160));

	std::size_t hit_index = 99;
	check(ff_preview_unique_hit(nullptr, 0, 0.5, 0.5, hit_index) == ff_preview_hit_result::no_hit);
	const ff_preview_rect_d unique_candidate[] = {{0.25, 0.25, 0.5, 0.5}};
	check(ff_preview_unique_hit(unique_candidate, 1, 0.5, 0.5, hit_index) == ff_preview_hit_result::unique_hit);
	check(hit_index == 0);
	hit_index = 99;
	check(ff_preview_unique_hit(unique_candidate, 1, 0.9, 0.9, hit_index) == ff_preview_hit_result::no_hit);
	check(hit_index == 99);
	const ff_preview_rect_d overlapping_candidates[] = {{0.1, 0.1, 0.5, 0.5}, {0.4, 0.4, 0.5, 0.5}};
	check(ff_preview_unique_hit(overlapping_candidates, 2, 0.45, 0.45, hit_index) ==
	      ff_preview_hit_result::ambiguous);
	check(hit_index == 99);
	const ff_preview_rect_d edge_candidates[] = {{0.1, 0.1, 0.3, 0.3}, {0.4, 0.1, 0.3, 0.3}};
	check(ff_preview_unique_hit(edge_candidates, 2, 0.4, 0.2, hit_index) == ff_preview_hit_result::ambiguous);
	const ff_preview_rect_d invalid_hit_candidates[] = {{0.25, 0.25, 0.5, 0.5}, {NAN, 0.0, 0.5, 0.5}};
	check(ff_preview_unique_hit(invalid_hit_candidates, 2, 0.5, 0.5, hit_index) == ff_preview_hit_result::no_hit);
	check(ff_preview_unique_hit(nullptr, 1, 0.5, 0.5, hit_index) == ff_preview_hit_result::no_hit);
	const ff_preview_rect_d malformed_candidates[] = {
		{-0.1, 0.25, 0.5, 0.5},  {0.75, 0.25, 0.5, 0.5}, {0.25, 0.25, 0.0, 0.5},
		{0.25, 0.25, 0.5, -0.1}, {0.25, 1.1, 0.1, 0.1},  {0.25, 0.25, INFINITY, 0.5},
	};
	for (const ff_preview_rect_d &malformed : malformed_candidates) {
		hit_index = 77;
		check(ff_preview_unique_hit(&malformed, 1, 0.5, 0.5, hit_index) == ff_preview_hit_result::no_hit);
		check(hit_index == 77);
	}
	check(ff_preview_unique_hit(unique_candidate, 1, -0.1, 0.5, hit_index) == ff_preview_hit_result::no_hit);
	check(ff_preview_unique_hit(unique_candidate, 1, 1.1, 0.5, hit_index) == ff_preview_hit_result::no_hit);
	check(ff_preview_unique_hit(unique_candidate, 1, NAN, 0.5, hit_index) == ff_preview_hit_result::no_hit);
	check(ff_preview_unique_hit(unique_candidate, 1, INFINITY, 0.5, hit_index) == ff_preview_hit_result::no_hit);

	double x = -1.0, y = -1.0;
	check(ff_preview_widget_point(128.0, 72.0, 256.0, 144.0, 1920, 1080, x, y));
	check(near(x, 0.5) && near(y, 0.5));
	check(ff_preview_widget_point(256.0, 144.0, 512.0, 288.0, 1920, 1080, x, y));
	check(near(x, 0.5) && near(y, 0.5)); // HiDPI-scaled dimensions preserve normalized hit position.
	check(!ff_preview_widget_point(4.0, 72.0, 400.0, 144.0, 1920, 1080, x, y));  // horizontal letterbox
	check(!ff_preview_widget_point(128.0, 4.0, 144.0, 256.0, 1920, 1080, x, y)); // vertical letterbox
	double zoom = 0.0;
	check(ff_preview_zoom_factor(ff_rect{0.0f, 0.0f, 1.0f, 1.0f}, 3840, 2160, 1280, 960, zoom));
	check(near(zoom, 1.0)); // Full-source framing remains 1x with aspect-fit padding.
	check(ff_preview_zoom_factor(ff_rect{0.3125f, 0.359375f, 0.375f, 0.28125f}, 3840, 2160, 1280, 960, zoom));
	check(near(zoom, 2.0)); // A 4:3 output crop from 16:9 input uses the crop's fit width.
	check(!ff_preview_zoom_factor(ff_rect{NAN, 0.0f, 0.5f, 0.5f}, 3840, 2160, 1920, 1080, zoom));
	check(!ff_preview_zoom_factor(ff_rect{0.0f, 0.0f, 1.0f, 1.0f}, 0, 2160, 1920, 1080, zoom));

	ff_rect source_crop{0.25f, 0.25f, 0.5f, 0.5f};
	ff_rect source_viewport{0.1f, 0.1f, 0.8f, 0.8f};
	ff_preview_rect_d mapped{};
	uint32_t source_preview_width = 1920, source_preview_height = 1080;
	check(ff_preview_set_full_source_transform(source_crop, source_viewport, source_preview_width,
						   source_preview_height, 3840, 2160));
	check(source_preview_width == 3840 && source_preview_height == 2160);
	check(ff_preview_valid_rect(source_crop) && source_crop.x == 0.0f && source_crop.y == 0.0f &&
	      source_crop.width == 1.0f && source_crop.height == 1.0f);
	check(ff_preview_valid_rect(source_viewport) && source_viewport.x == 0.0f && source_viewport.y == 0.0f &&
	      source_viewport.width == 1.0f && source_viewport.height == 1.0f);
	const ff_rect source_candidate{0.25f, 0.25f, 0.25f, 0.25f};
	check(ff_preview_candidate_rect(source_crop, source_viewport, source_candidate, mapped));
	check(near(mapped.x, 0.25) && near(mapped.y, 0.25) && near(mapped.width, 0.25) && near(mapped.height, 0.25));
	check(!ff_preview_set_full_source_transform(source_crop, source_viewport, source_preview_width,
						    source_preview_height, 0, 2160));

	const ff_rect crop{0.25f, 0.25f, 0.5f, 0.5f};
	const ff_rect viewport{0.0f, 0.125f, 1.0f, 0.75f};
	const ff_rect candidate{0.5f, 0.375f, 0.25f, 0.25f};
	check(ff_preview_candidate_rect(crop, viewport, candidate, mapped));
	check(near(mapped.x, 0.5) && near(mapped.y, 0.3125));
	check(near(mapped.width, 0.5) && near(mapped.height, 0.375));
	ff_preview_rect_d mapped_overlap[2]{};
	check(ff_preview_candidate_rect(crop, viewport, ff_rect{0.35f, 0.35f, 0.3f, 0.3f}, mapped_overlap[0]));
	check(ff_preview_candidate_rect(crop, viewport, ff_rect{0.45f, 0.45f, 0.25f, 0.25f}, mapped_overlap[1]));
	check(ff_preview_unique_hit(mapped_overlap, 2, 0.5, 0.5, hit_index) == ff_preview_hit_result::ambiguous);

	const ff_rect edge_candidate{0.0f, 0.25f, 0.375f, 0.25f};
	check(ff_preview_candidate_rect(crop, viewport, edge_candidate, mapped));
	check(near(mapped.x, 0.0) && near(mapped.width, 0.25)); // clipped at crop edge

	const ff_rect outside_candidate{0.0f, 0.0f, 0.1f, 0.1f};
	check(!ff_preview_candidate_rect(crop, viewport, outside_candidate, mapped));
	const ff_rect alternate_stage_candidate{0.05f, 0.35f, 0.1f, 0.1f};
	check(!ff_preview_candidate_rect(crop, viewport, alternate_stage_candidate, mapped));
	ff_rect wide_crop = crop;
	ff_rect wide_viewport = viewport;
	uint32_t wide_width = 1920, wide_height = 1080;
	check(ff_preview_set_full_source_transform(wide_crop, wide_viewport, wide_width, wide_height, 3840, 2160));
	check(ff_preview_candidate_rect(wide_crop, wide_viewport, alternate_stage_candidate, mapped));
	check(near(mapped.x, 0.05) && near(mapped.y, 0.35));
	const ff_rect invalid_candidate{0.95f, 0.5f, 0.1f, 0.2f};
	check(!ff_preview_candidate_rect(crop, viewport, invalid_candidate, mapped));
	const ff_rect non_finite{NAN, 0.2f, 0.1f, 0.1f};
	check(!ff_preview_candidate_rect(crop, viewport, non_finite, mapped));
	check(!ff_preview_widget_point(INFINITY, 1.0, 100.0, 100.0, 1920, 1080, x, y));
	return 0;
}
