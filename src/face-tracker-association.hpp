#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

struct ff_tracker_box_i
{
	int x0;
	int y0;
	int x1;
	int y1;
};

/// Tracker scores are ranking evidence only when they are finite and positive.
inline bool ff_tracker_score_is_usable(float score)
{
	return std::isfinite(score) && score > 0.0f;
}

/// Conservative overlap check used to avoid creating a new dlib tracker for a
/// detection already represented by an active candidate. Requiring half of
/// the smaller box to overlap favors missed reacquisition over identity swaps.
inline bool ff_tracker_boxes_match(const ff_tracker_box_i &left, const ff_tracker_box_i &right)
{
	const int64_t left_width = static_cast<int64_t>(left.x1) - left.x0;
	const int64_t left_height = static_cast<int64_t>(left.y1) - left.y0;
	const int64_t right_width = static_cast<int64_t>(right.x1) - right.x0;
	const int64_t right_height = static_cast<int64_t>(right.y1) - right.y0;
	if (left_width <= 0 || left_height <= 0 || right_width <= 0 || right_height <= 0)
		return false;
	const int64_t maximum = std::numeric_limits<int64_t>::max();
	if (left_width > maximum / left_height || right_width > maximum / right_height)
		return false;

	const int64_t overlap_width =
		std::max<int64_t>(0, static_cast<int64_t>(std::min(left.x1, right.x1)) - std::max(left.x0, right.x0));
	const int64_t overlap_height =
		std::max<int64_t>(0, static_cast<int64_t>(std::min(left.y1, right.y1)) - std::max(left.y0, right.y0));
	if (overlap_height > 0 && overlap_width > maximum / overlap_height)
		return false;
	const int64_t overlap_area = overlap_width * overlap_height;
	const int64_t smaller_area = std::min(left_width * left_height, right_width * right_height);
	return overlap_area >= smaller_area / 2 + smaller_area % 2;
}
