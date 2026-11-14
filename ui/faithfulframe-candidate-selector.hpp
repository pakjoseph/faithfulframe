#pragma once

namespace faithfulframe::ui {

template<typename Refresh> inline bool refresh_candidate_selector_model(bool popup_visible, Refresh refresh)
{
	if (popup_visible)
		return false;
	refresh();
	return true;
}

} // namespace faithfulframe::ui
