#pragma once

#include <cstdint>

enum class faithfulframe_crop_input : uint8_t {
	legacy_aggregate,
	safe_session,
};

/// Selects the sole crop source before any OBS output is drawn. Once enabled,
/// the safety policy owns the crop path for selected, held, and safe-wide frames.
constexpr faithfulframe_crop_input faithfulframe_select_crop_input(bool safety_policy_enabled) noexcept
{
	return safety_policy_enabled ? faithfulframe_crop_input::safe_session
				     : faithfulframe_crop_input::legacy_aggregate;
}
