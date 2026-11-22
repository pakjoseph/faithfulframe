#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

/// Return the next detector tick without performing an invalid float-to-int
/// conversion when OBS supplies a zero, non-finite, or extremely small frame
/// delta. Invalid timing defers work by one frame; valid timing preserves the
/// manager's existing truncation behavior and saturates before integer overflow.
inline int faithfulframe_detector_schedule_next_tick(int current_tick, float interval_seconds,
						     float elapsed_seconds) noexcept
{
	constexpr int maximum_tick = std::numeric_limits<int>::max();
	if (current_tick == maximum_tick)
		return maximum_tick;

	if (!std::isfinite(interval_seconds) || interval_seconds <= 0.0f || !std::isfinite(elapsed_seconds) ||
	    elapsed_seconds <= 0.0f)
		return current_tick + 1;

	// Keep the original float division so ordinary OBS frame deltas retain the
	// same truncation result (for example, 2.0f / 0.1f schedules 20 ticks).
	const float interval_ticks = interval_seconds / elapsed_seconds;
	if (std::isinf(interval_ticks) && interval_ticks > 0.0f)
		return maximum_tick;
	if (interval_ticks == 0.0f)
		return current_tick;
	if (!std::isfinite(interval_ticks) || interval_ticks < 0.0f)
		return current_tick + 1;

	const std::int64_t available_ticks = static_cast<std::int64_t>(maximum_tick) - current_tick;
	const double whole_ticks = std::trunc(static_cast<double>(interval_ticks));
	if (whole_ticks >= static_cast<double>(available_ticks))
		return maximum_tick;

	return static_cast<int>(static_cast<std::int64_t>(current_tick) + static_cast<std::int64_t>(whole_ticks));
}
