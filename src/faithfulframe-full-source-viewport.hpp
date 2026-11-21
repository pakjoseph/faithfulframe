#pragma once

#include <cstdint>

#include "faithfulframe_ffi.h"

/// Fit the complete source inside the output, leaving padding where aspect
/// ratios differ. Safe-wide framing must never discard part of the stage.
inline ff_rect faithfulframe_full_source_viewport(uint32_t source_width, uint32_t source_height, uint32_t output_width,
						  uint32_t output_height) noexcept
{
	if (!source_width || !source_height || !output_width || !output_height)
		return ff_rect{0.0f, 0.0f, 1.0f, 1.0f};

	const double source_aspect = static_cast<double>(source_width) / source_height;
	const double output_aspect = static_cast<double>(output_width) / output_height;
	if (source_aspect > output_aspect) {
		const float height = static_cast<float>(output_aspect / source_aspect);
		return ff_rect{0.0f, (1.0f - height) * 0.5f, 1.0f, height};
	}
	if (source_aspect < output_aspect) {
		const float width = static_cast<float>(source_aspect / output_aspect);
		return ff_rect{(1.0f - width) * 0.5f, 0.0f, width, 1.0f};
	}
	return ff_rect{0.0f, 0.0f, 1.0f, 1.0f};
}
