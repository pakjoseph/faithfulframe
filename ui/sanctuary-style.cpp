#include "sanctuary-style.hpp"

namespace sanctuary::style {

colors colors_from_palette(const QPalette &palette)
{
	return {
		palette.color(QPalette::Window),
		palette.color(QPalette::Base),
		palette.color(QPalette::WindowText),
		palette.color(QPalette::Disabled, QPalette::WindowText),
		palette.color(QPalette::Mid),
		palette.color(QPalette::Highlight),
		palette.color(QPalette::Highlight),
		QColor("#287A4B"),
		QColor("#A66300"),
		palette.color(QPalette::BrightText),
	};
}

QString control_stylesheet(const colors &tokens)
{
	// Only layout/state geometry is fixed. Color roles come from the host palette
	// so OBS light, dark, and high-contrast themes retain their character.
	return QString("QPushButton { min-height: %1px; border-radius: %2px; "
		       "padding: 0 %3px; border: 1px solid %4; } "
		       "QPushButton:focus { border: 2px solid %5; } "
		       "QPushButton:disabled { color: %6; }")
		.arg(control_standard_height)
		.arg(radius_medium)
		.arg(space_3)
		.arg(tokens.border.name())
		.arg(tokens.focus.name())
		.arg(tokens.text_muted.name());
}

} // namespace sanctuary::style
