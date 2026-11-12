#include "faithfulframe-crop-selector.hpp"

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
	    faithfulframe_session_crop_is_authorized(FF_CROP_TRACK, static_cast<ff_state>(99), true) ||
	    faithfulframe_session_crop_is_authorized(static_cast<ff_crop_action>(99), FF_STATE_LOCKED, true))
		return 4;
	if (!faithfulframe_session_crop_is_authorized(FF_CROP_HOLD, FF_STATE_MANUAL, false) ||
	    !faithfulframe_session_crop_is_authorized(FF_CROP_WIDE, FF_STATE_LOST, false) ||
	    !faithfulframe_session_crop_is_authorized(FF_CROP_NONE, FF_STATE_FROZEN, false))
		return 5;
	return 0;
}
