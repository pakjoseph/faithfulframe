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

/// Apply dlib's peak-to-side-lobe ratio (PSR) evidence without allowing an
/// invalid or non-positive response to preserve or restore tracker confidence.
/// dlib documents larger PSR values as higher confidence; non-positive values
/// provide no positive peak evidence, so this tracker must stay unusable.
inline float ff_tracker_confidence_after_psr(float current_score, double psr, double psr_max, double psr_min,
					     double elapsed_seconds)
{
	if (!ff_tracker_score_is_usable(current_score) || !std::isfinite(psr) || psr <= 0.0 ||
	    !std::isfinite(psr_max) || psr_max <= 0.0 || !std::isfinite(psr_min) || psr_min <= 0.0 ||
	    !std::isfinite(elapsed_seconds) || elapsed_seconds < 0.0)
		return 0.0f;

	const double ratio = psr_max / psr_min;
	const double decay = ratio * elapsed_seconds;
	const double next_score = static_cast<double>(current_score) / (1.0 + decay);
	if (!std::isfinite(ratio) || ratio <= 0.0 || !std::isfinite(decay) || decay < 0.0 ||
	    !std::isfinite(next_score) || next_score <= 0.0 || next_score > current_score)
		return 0.0f;
	return static_cast<float>(next_score);
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
