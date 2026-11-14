#pragma once

#include <QEvent>
#include <Qt>

namespace faithfulframe {
namespace ui {

inline constexpr Qt::FocusPolicy selector_focus_policy = Qt::StrongFocus;

inline void ignore_selector_wheel_event(QEvent *event) noexcept
{
	if (event)
		event->ignore();
}

} // namespace ui
} // namespace faithfulframe
