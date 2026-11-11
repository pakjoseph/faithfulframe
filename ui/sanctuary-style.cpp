#include "sanctuary-style.hpp"

namespace sanctuary::style {

colors colors_from_palette(const QPalette &palette)
{
	return {
		palette.color(QPalette::Button),
		palette.color(QPalette::Midlight),
		palette.color(QPalette::ButtonText),
		palette.color(QPalette::Disabled, QPalette::ButtonText),
		palette.color(QPalette::Mid),
		palette.color(QPalette::Highlight),
		palette.color(QPalette::Highlight),
		palette.color(QPalette::HighlightedText),
		QColor("#287A4B"),
		QColor("#A66300"),
		palette.color(QPalette::BrightText),
	};
}

QString control_stylesheet(const colors &tokens)
{
	// Geometry is consistent while interaction colors follow the host palette,
	// including its foreground color for selected/pressed states.
	return QString("QPushButton { color: %6; background-color: %7; min-height: %1px; border-radius: %2px; "
		       "padding: 0 %3px; border: 1px solid %4; } "
		       "QPushButton:hover:!disabled { background-color: %8; border-color: %9; } "
		       "QPushButton:pressed:!disabled, QPushButton:checked:!disabled { color: %10; "
		       "background-color: %9; border-color: %9; } "
		       "QPushButton:focus { border: 2px solid %5; } "
		       "QPushButton:disabled { color: %11; }")
		.arg(control_standard_height)
		.arg(radius_medium)
		.arg(space_3)
		.arg(tokens.border.name())
		.arg(tokens.focus.name())
		.arg(tokens.text.name())
		.arg(tokens.surface.name())
		.arg(tokens.surface_raised.name())
		.arg(tokens.accent.name())
		.arg(tokens.accent_text.name())
		.arg(tokens.text_muted.name());
}

} // namespace sanctuary::style
