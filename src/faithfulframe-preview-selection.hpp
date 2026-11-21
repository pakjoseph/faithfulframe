#pragma once

#include <array>
#include <cstddef>

#include "faithfulframe-candidate-limits.hpp"
#include "faithfulframe-preview-geometry.hpp"
#include "faithfulframe-preview-render-stamp.hpp"

enum class ff_preview_click_result : std::uint8_t {
	selected,
	stale_preview,
	outside_image,
	invalid_geometry,
	no_hit,
	ambiguous,
	too_many_candidates,
};

/// Resolve a click against the exact preview frame and candidate list that the
/// operator saw. `selected_index` is an index into `source_rectangles` and is
/// written only when one candidate is hit uniquely.
inline ff_preview_click_result ff_preview_select_candidate(const faithfulframe_preview_render_stamp &rendered_stamp,
							   const faithfulframe_preview_render_stamp &current_stamp,
							   double px, double py, double widget_width,
							   double widget_height, const ff_rect *source_rectangles,
							   std::size_t candidate_count, std::size_t &selected_index)
{
	if (!faithfulframe_preview_render_stamp_matches(rendered_stamp, current_stamp))
		return ff_preview_click_result::stale_preview;
	if (candidate_count > faithfulframe_max_candidates)
		return ff_preview_click_result::too_many_candidates;
	if (candidate_count && !source_rectangles)
		return ff_preview_click_result::no_hit;

	double x = 0.0;
	double y = 0.0;
	if (!ff_preview_widget_point(px, py, widget_width, widget_height, current_stamp.output_width,
				     current_stamp.output_height, x, y))
		return ff_preview_click_result::outside_image;

	std::array<ff_preview_rect_d, faithfulframe_max_candidates> visible_rectangles{};
	std::array<std::size_t, faithfulframe_max_candidates> source_indices{};
	const ff_rect crop{current_stamp.crop.x, current_stamp.crop.y, current_stamp.crop.width,
			   current_stamp.crop.height};
	const ff_rect viewport{current_stamp.viewport.x, current_stamp.viewport.y, current_stamp.viewport.width,
			       current_stamp.viewport.height};
	std::size_t visible_count = 0;
	for (std::size_t i = 0; i < candidate_count; ++i) {
		if (!ff_preview_valid_rect(source_rectangles[i]))
			return ff_preview_click_result::invalid_geometry;
		ff_preview_rect_d visible{};
		if (!ff_preview_candidate_rect(crop, viewport, source_rectangles[i], visible))
			continue;
		visible_rectangles[visible_count] = visible;
		source_indices[visible_count++] = i;
	}

	std::size_t visible_index = 0;
	const ff_preview_hit_result hit =
		ff_preview_unique_hit(visible_rectangles.data(), visible_count, x, y, visible_index);
	if (hit == ff_preview_hit_result::ambiguous)
		return ff_preview_click_result::ambiguous;
	if (hit != ff_preview_hit_result::unique_hit)
		return ff_preview_click_result::no_hit;
	selected_index = source_indices[visible_index];
	return ff_preview_click_result::selected;
}
