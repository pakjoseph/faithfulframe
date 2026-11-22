#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

inline constexpr uint64_t faithfulframe_detector_interval_min_ms = 1000;
inline constexpr uint64_t faithfulframe_detector_interval_default_ms = 2000;
inline constexpr uint64_t faithfulframe_detector_interval_step_ms = 250;

/// Keep the configurable cadence inside the tested control range. Invalid or
/// old profile values fall back to the existing two-second interval.
inline uint64_t faithfulframe_sanitize_detector_interval_ms(int64_t interval_ms) noexcept
{
	if (interval_ms < static_cast<int64_t>(faithfulframe_detector_interval_min_ms) ||
	    interval_ms > static_cast<int64_t>(faithfulframe_detector_interval_default_ms) ||
	    (interval_ms - static_cast<int64_t>(faithfulframe_detector_interval_min_ms)) %
			    static_cast<int64_t>(faithfulframe_detector_interval_step_ms) !=
		    0)
		return faithfulframe_detector_interval_default_ms;
	return static_cast<uint64_t>(interval_ms);
}

inline float faithfulframe_detector_interval_seconds(uint64_t interval_ms) noexcept
{
	return static_cast<float>(interval_ms) / 1000.0f;
}

/// Return the next detector tick without performing an invalid float-to-int
/// conversion when OBS supplies a zero, non-finite, or extremely small frame
/// delta. Invalid timing defers work by one frame; valid timing preserves the
/// manager's existing truncation behavior and saturates before integer overflow.
inline uint64_t faithfulframe_next_frame_tick(uint64_t current_tick) noexcept
{
	constexpr uint64_t maximum_tick = std::numeric_limits<uint64_t>::max();
	return current_tick == maximum_tick ? current_tick : current_tick + 1;
}

inline uint64_t faithfulframe_detector_schedule_next_tick(uint64_t current_tick, float interval_seconds,
							  float elapsed_seconds) noexcept
{
	constexpr uint64_t maximum_tick = std::numeric_limits<uint64_t>::max();
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

	const double whole_ticks = std::trunc(static_cast<double>(interval_ticks));
	// 2^64 is exactly representable as a double and is the first value that
	// cannot be converted to uint64_t without an out-of-range conversion.
	constexpr double uint64_tick_limit = 18446744073709551616.0;
	if (whole_ticks >= uint64_tick_limit)
		return maximum_tick;

	const uint64_t tick_increment = static_cast<uint64_t>(whole_ticks);
	const uint64_t available_ticks = maximum_tick - current_tick;
	if (tick_increment >= available_ticks)
		return maximum_tick;

	return current_tick + tick_increment;
}
