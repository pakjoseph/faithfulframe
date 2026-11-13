#include "faithfulframe-crop-selector.hpp"

#include <limits>

int main()
{
	if (faithfulframe_select_crop_input(false) != faithfulframe_crop_input::legacy_aggregate)
		return 1;
	if (faithfulframe_select_crop_input(true) != faithfulframe_crop_input::safe_session)
		return 2;
	if (!faithfulframe_session_crop_is_authorized(FF_CROP_TRACK, FF_STATE_LOCKED, true))
		return 3;
	if (faithfulframe_session_crop_is_authorized(FF_CROP_TRACK, FF_STATE_MANUAL, true) ||
	    faithfulframe_session_crop_is_authorized(FF_CROP_TRACK, FF_STATE_LOCKED, false) ||
	    faithfulframe_session_crop_is_authorized(FF_CROP_TRACK, 0xffffffffu, true) ||
	    faithfulframe_session_crop_is_authorized(99u, FF_STATE_LOCKED, true))
		return 4;
	if (!faithfulframe_session_crop_is_authorized(FF_CROP_HOLD, FF_STATE_MANUAL, false) ||
	    !faithfulframe_session_crop_is_authorized(FF_CROP_WIDE, FF_STATE_LOST, false) ||
	    !faithfulframe_session_crop_is_authorized(FF_CROP_NONE, FF_STATE_FROZEN, false))
		return 5;
	if (!faithfulframe_valid_normalized_rect(ff_rect{0.0f, 0.0f, 1.0f, 1.0f}) ||
	    !faithfulframe_valid_normalized_rect(ff_rect{0.1f, 0.2f, 0.3f, 0.4f}))
		return 6;
	if (faithfulframe_valid_normalized_rect(ff_rect{-0.1f, 0.0f, 0.5f, 0.5f}) ||
	    faithfulframe_valid_normalized_rect(ff_rect{0.0f, -0.1f, 0.5f, 0.5f}) ||
	    faithfulframe_valid_normalized_rect(ff_rect{0.0f, 0.0f, 0.0f, 0.5f}) ||
	    faithfulframe_valid_normalized_rect(ff_rect{0.0f, 0.0f, 0.5f, -0.1f}) ||
	    faithfulframe_valid_normalized_rect(ff_rect{0.8f, 0.0f, 0.3f, 0.5f}) ||
	    faithfulframe_valid_normalized_rect(ff_rect{0.0f, 0.8f, 0.5f, 0.3f}) ||
	    faithfulframe_valid_normalized_rect(ff_rect{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.5f, 0.5f}) ||
	    faithfulframe_valid_normalized_rect(ff_rect{0.0f, std::numeric_limits<float>::infinity(), 0.5f, 0.5f}) ||
	    faithfulframe_valid_normalized_rect(ff_rect{0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 0.5f}) ||
	    faithfulframe_valid_normalized_rect(ff_rect{0.0f, 0.0f, 0.5f, std::numeric_limits<float>::infinity()}))
		return 7;
	if (faithfulframe_safety_policy_enabled_by_default ||
	    faithfulframe_select_crop_input(faithfulframe_safety_policy_enabled_by_default) !=
		    faithfulframe_crop_input::legacy_aggregate)
		return 8;
	return 0;
}
