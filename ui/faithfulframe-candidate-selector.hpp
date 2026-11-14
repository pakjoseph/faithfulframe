#pragma once

#include <QAbstractItemView>
#include <QComboBox>

namespace faithfulframe::ui {

inline bool candidate_selector_model_can_refresh(const QComboBox *selector)
{
	return !selector || !selector->view() || !selector->view()->isVisible();
}

} // namespace faithfulframe::ui
