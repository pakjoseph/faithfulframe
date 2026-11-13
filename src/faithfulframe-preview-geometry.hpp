#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "faithfulframe_ffi.h"

struct ff_preview_rect_d
{
	double x;
	double y;
	double width;
	double height;
};

enum class ff_preview_hit_result : std::uint8_t {
	no_hit,
	unique_hit,
	ambiguous,
};

/// Select exactly one normalized preview rectangle containing a point.
/// Inclusive edges are intentional: touching or overlapping boxes are
/// ambiguous at their shared boundary and must not choose arbitrarily. The
/// output index is changed only when the result is unique.
inline ff_preview_hit_result ff_preview_unique_hit(const ff_preview_rect_d *rectangles, std::size_t count, double x,
						   double y, std::size_t &hit_index)
{
	if ((count && !rectangles) || !std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 ||
	    y > 1.0)
		return ff_preview_hit_result::no_hit;
	std::size_t hits = 0;
	std::size_t unique_index = 0;
	for (std::size_t i = 0; i < count; ++i) {
		const auto &rectangle = rectangles[i];
		if (!std::isfinite(rectangle.x) || !std::isfinite(rectangle.y) || !std::isfinite(rectangle.width) ||
		    !std::isfinite(rectangle.height) || rectangle.x < 0.0 || rectangle.y < 0.0 ||
		    rectangle.width <= 0.0 || rectangle.height <= 0.0 || rectangle.x + rectangle.width > 1.0 ||
		    rectangle.y + rectangle.height > 1.0)
			return ff_preview_hit_result::no_hit;
		if (x >= rectangle.x && x <= rectangle.x + rectangle.width && y >= rectangle.y &&
		    y <= rectangle.y + rectangle.height) {
			unique_index = i;
			if (++hits > 1)
				return ff_preview_hit_result::ambiguous;
		}
	}
	if (hits != 1)
		return ff_preview_hit_result::no_hit;
	hit_index = unique_index;
	return ff_preview_hit_result::unique_hit;
}

inline bool ff_preview_valid_rect(const ff_rect &rect)
{
	return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
	       std::isfinite(rect.height) && rect.x >= 0.0f && rect.y >= 0.0f && rect.width > 0.0f &&
	       rect.height > 0.0f && rect.x + rect.width <= 1.0f && rect.y + rect.height <= 1.0f;
}

inline bool ff_preview_observation_is_fresh(uint64_t observed_at_ms, uint64_t now_ms, uint64_t maximum_age_ms)
{
	return observed_at_ms != 0 && observed_at_ms <= now_ms && now_ms - observed_at_ms <= maximum_age_ms;
}

inline bool ff_preview_source_geometry_matches(uint32_t bound_width, uint32_t bound_height, uint32_t live_width,
					       uint32_t live_height)
{
	return bound_width != 0 && bound_height != 0 && bound_width == live_width && bound_height == live_height;
}

/// Estimate displayed digital zoom from a source-space crop. Full-source
/// framing remains 1x even when aspect-fit padding is used.
inline bool ff_preview_zoom_factor(const ff_rect &crop, uint32_t source_width, uint32_t source_height,
				   uint32_t output_width, uint32_t output_height, double &zoom)
{
	if (!ff_preview_valid_rect(crop) || !source_width || !source_height || !output_width || !output_height)
		return false;
	if (crop.x == 0.0f && crop.y == 0.0f && crop.width == 1.0f && crop.height == 1.0f) {
		zoom = 1.0;
		return true;
	}
	const double source_aspect = static_cast<double>(source_width) / source_height;
	const double output_aspect = static_cast<double>(output_width) / output_height;
	const double base_width = std::min(output_aspect / source_aspect, 1.0);
	const double result = base_width / crop.width;
	if (!std::isfinite(result) || result < 1.0)
		return false;
	zoom = result;
	return true;
}

/// Map a normalized source candidate into the displayed output image, clipping
/// to the source crop and output viewport. All inputs are normalized rectangles.
inline bool ff_preview_candidate_rect(const ff_rect &crop, const ff_rect &viewport, const ff_rect &candidate,
				      ff_preview_rect_d &output)
{
	if (!ff_preview_valid_rect(crop) || !ff_preview_valid_rect(viewport) || !ff_preview_valid_rect(candidate))
		return false;
	double left = viewport.x + (candidate.x - crop.x) / crop.width * viewport.width;
	double top = viewport.y + (candidate.y - crop.y) / crop.height * viewport.height;
	double right = viewport.x + (candidate.x + candidate.width - crop.x) / crop.width * viewport.width;
	double bottom = viewport.y + (candidate.y + candidate.height - crop.y) / crop.height * viewport.height;
	left = std::max(left, static_cast<double>(viewport.x));
	top = std::max(top, static_cast<double>(viewport.y));
	right = std::min(right, static_cast<double>(viewport.x + viewport.width));
	bottom = std::min(bottom, static_cast<double>(viewport.y + viewport.height));
	if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom) ||
	    right <= left || bottom <= top)
		return false;
	output = ff_preview_rect_d{left, top, right - left, bottom - top};
	return true;
}

/// Use the complete source as the preview image when the monitor displays the
/// untracked camera feed. Its canvas then has the source's aspect ratio.
inline bool ff_preview_set_full_source_transform(ff_rect &crop, ff_rect &viewport, uint32_t &output_width,
						 uint32_t &output_height, uint32_t source_width, uint32_t source_height)
{
	if (!source_width || !source_height)
		return false;
	crop = ff_rect{0.0f, 0.0f, 1.0f, 1.0f};
	viewport = ff_rect{0.0f, 0.0f, 1.0f, 1.0f};
	output_width = source_width;
	output_height = source_height;
	return true;
}

/// Convert a widget-local point into normalized output-image coordinates after
/// aspect-fit letterboxing. Logical or physical pixels work as long as point
/// and widget dimensions use the same units.
inline bool ff_preview_widget_point(double px, double py, double widget_width, double widget_height,
				    uint32_t image_width, uint32_t image_height, double &x, double &y)
{
	if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(widget_width) || !std::isfinite(widget_height) ||
	    widget_width <= 0.0 || widget_height <= 0.0 || !image_width || !image_height)
		return false;
	const double image_aspect = static_cast<double>(image_width) / image_height;
	if (!std::isfinite(image_aspect) || image_aspect <= 0.0)
		return false;
	double draw_width = widget_width;
	double draw_height = widget_height;
	if (widget_width / widget_height > image_aspect)
		draw_width = widget_height * image_aspect;
	else
		draw_height = widget_width / image_aspect;
	const double left = (widget_width - draw_width) * 0.5;
	const double top = (widget_height - draw_height) * 0.5;
	if (px < left || px > left + draw_width || py < top || py > top + draw_height)
		return false;
	x = (px - left) / draw_width;
	y = (py - top) / draw_height;
	return std::isfinite(x) && std::isfinite(y) && x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0;
}
