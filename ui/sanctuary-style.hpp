#pragma once

// FaithfulFrame native Qt design tokens. Keep this layer small: it adapts the
// active OBS/Qt palette instead of imposing a separate application theme.

#include <QColor>
#include <QPalette>
#include <QString>

namespace sanctuary::style {

inline constexpr int space_1 = 4;
inline constexpr int space_2 = 8;
inline constexpr int space_3 = 12;
inline constexpr int space_4 = 16;
inline constexpr int space_5 = 20;
inline constexpr int space_6 = 24;

inline constexpr int radius_small = 4;
inline constexpr int radius_medium = 8;
inline constexpr int radius_large = 12;

inline constexpr int control_compact_height = 28;
inline constexpr int control_standard_height = 36;
inline constexpr int control_primary_height = 44;

struct colors
{
	QColor surface;
	QColor surface_raised;
	QColor text;
	QColor text_muted;
	QColor border;
	QColor focus;
	QColor accent;
	QColor accent_text;
	QColor success;
	QColor warning;
	QColor danger;
};

colors colors_from_palette(const QPalette &palette);
QString control_stylesheet(const colors &tokens);

} // namespace sanctuary::style
