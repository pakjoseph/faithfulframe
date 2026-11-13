#pragma once

#include <cmath>
#include <cstdint>

#include "faithfulframe_ffi.h"

enum class faithfulframe_crop_input : uint8_t {
	legacy_aggregate,
	safe_session,
};

/// Keep automatic safety-policy tracking opt-in until production acceptance gates pass.
constexpr bool faithfulframe_safety_policy_enabled_by_default = false;

/// Reject malformed normalized rectangles before they reach OBS rendering.
inline bool faithfulframe_valid_normalized_rect(const ff_rect &rect) noexcept
{
	return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
	       std::isfinite(rect.height) && rect.x >= 0.0f && rect.y >= 0.0f && rect.width > 0.0f &&
	       rect.height > 0.0f && rect.x + rect.width <= 1.0f && rect.y + rect.height <= 1.0f;
}

/// Selects the sole crop source before any OBS output is drawn. Once enabled,
/// the safety policy owns the crop path for selected, held, and safe-wide frames.
constexpr faithfulframe_crop_input faithfulframe_select_crop_input(bool safety_policy_enabled) noexcept
{
	return safety_policy_enabled ? faithfulframe_crop_input::safe_session
				     : faithfulframe_crop_input::legacy_aggregate;
}

/// Track crops require an explicit lock. Hold, wide, and none are explicit
/// session framing decisions. Unknown actions or states fail closed.
constexpr bool faithfulframe_session_crop_is_authorized(uint32_t action, uint32_t state,
							bool has_selected_target) noexcept
{
	if (state > FF_STATE_LOST)
		return false;
	switch (action) {
	case FF_CROP_TRACK:
		return state == FF_STATE_LOCKED && has_selected_target;
	case FF_CROP_HOLD:
	case FF_CROP_WIDE:
	case FF_CROP_NONE:
		return true;
	default:
		return false;
	}
}
