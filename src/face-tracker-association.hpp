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
inline float ff_tracker_confidence_from_psr(double psr, double psr_max)
{
	if (!std::isfinite(psr) || psr <= 0.0 || !std::isfinite(psr_max) || psr_max <= 0.0 || psr > psr_max)
		return 0.0f;

	const double normalized = psr / psr_max;
	if (!std::isfinite(normalized) || normalized <= 0.0 || normalized > 1.0)
		return 0.0f;
	return static_cast<float>(normalized);
}

/// A detector match renews candidate attention. Without a match, attenuate it
/// using the best observed overlap and the existing 10% overlap floor.
inline float ff_tracker_attention_after_detection(float current_attention, float overlap_fraction, bool detector_match)
{
	if (!std::isfinite(current_attention) || current_attention < 0.0f || current_attention > 1.0f ||
	    !std::isfinite(overlap_fraction))
		return 0.0f;
	if (detector_match)
		return 1.0f;
	const float overlap = std::clamp(overlap_fraction, 0.1f, 1.0f);
	const float next_attention = current_attention * std::pow(overlap, 0.1f);
	return std::isfinite(next_attention) ? std::clamp(next_attention, 0.0f, 1.0f) : 0.0f;
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
