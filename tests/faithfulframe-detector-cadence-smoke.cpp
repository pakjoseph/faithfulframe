#include "faithfulframe-detector-cadence.hpp"
#include "faithfulframe-candidate-limits.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

static void check_impl(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "detector cadence check failed at line %d\n", line);
		std::abort();
	}
}

#define check(condition) check_impl((condition), __LINE__)

int main()
{
	using tick_t = uint64_t;
	const tick_t maximum_tick = std::numeric_limits<tick_t>::max();
	check(faithfulframe_sanitize_detector_interval_ms(1000) == 1000);
	check(faithfulframe_sanitize_detector_interval_ms(1250) == 1250);
	check(faithfulframe_sanitize_detector_interval_ms(1500) == 1500);
	check(faithfulframe_sanitize_detector_interval_ms(1750) == 1750);
	check(faithfulframe_sanitize_detector_interval_ms(2000) == 2000);
	check(faithfulframe_sanitize_detector_interval_ms(999) == faithfulframe_detector_interval_default_ms);
	check(faithfulframe_sanitize_detector_interval_ms(1125) == faithfulframe_detector_interval_default_ms);
	check(faithfulframe_sanitize_detector_interval_ms(2001) == faithfulframe_detector_interval_default_ms);
	check(faithfulframe_detector_interval_seconds(1000) == 1.0f);
	check(faithfulframe_detector_confirmation_max_age_ms(1000) == 1500);
	check(faithfulframe_detector_confirmation_max_age_ms(1250) == 1750);
	check(faithfulframe_detector_confirmation_max_age_ms(1500) == 2000);
	check(faithfulframe_detector_confirmation_max_age_ms(1750) == 2250);
	check(faithfulframe_detector_confirmation_max_age_ms(2000) == 2500);
	check(faithfulframe_detector_confirmation_max_age_ms(0) == faithfulframe_candidate_max_age_ms);
	check(faithfulframe_detector_confirmation_max_age_ms(maximum_tick - faithfulframe_candidate_max_age_ms) ==
	      maximum_tick);
	check(faithfulframe_detector_confirmation_max_age_ms(maximum_tick - faithfulframe_candidate_max_age_ms + 1) ==
	      maximum_tick);
	check(faithfulframe_detector_confirmation_is_fresh(1000, 1500, 500));
	check(!faithfulframe_detector_confirmation_is_fresh(1000, 1501, 500));
	check(!faithfulframe_detector_confirmation_is_fresh(1501, 1500, 500));
	check(!faithfulframe_detector_confirmation_is_fresh(0, 0, 500));
	check(faithfulframe_next_frame_tick(0) == 1);
	check(faithfulframe_next_frame_tick(maximum_tick - 1) == maximum_tick);
	check(faithfulframe_next_frame_tick(maximum_tick) == maximum_tick);

	check(faithfulframe_detector_schedule_next_tick(0, 2.0f, 0.5f) == 4);
	check(faithfulframe_detector_schedule_next_tick(100, 1.0f, 0.1f) == 110);
	check(faithfulframe_detector_schedule_next_tick(100, 2.0f, 0.1f) == 120);
	check(faithfulframe_detector_schedule_next_tick(7, 2.0f, 3.0f) == 7);

	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float infinity = std::numeric_limits<float>::infinity();
	const float smallest_positive = std::numeric_limits<float>::denorm_min();
	const float largest_finite = std::numeric_limits<float>::max();
	check(faithfulframe_detector_schedule_next_tick(7, 2.0f, 0.0f) == 8);
	check(faithfulframe_detector_schedule_next_tick(7, 2.0f, -0.1f) == 8);
	check(faithfulframe_detector_schedule_next_tick(7, 2.0f, nan) == 8);
	check(faithfulframe_detector_schedule_next_tick(7, 2.0f, infinity) == 8);
	check(faithfulframe_detector_schedule_next_tick(7, nan, 0.1f) == 8);
	check(faithfulframe_detector_schedule_next_tick(7, 0.0f, 0.1f) == 8);
	check(faithfulframe_detector_schedule_next_tick(7, smallest_positive, largest_finite) == 7);
	check(faithfulframe_detector_schedule_next_tick(7, 2.0f, smallest_positive) == maximum_tick);
	check(faithfulframe_detector_schedule_next_tick(7, largest_finite, 1.0f) == maximum_tick);
	check(faithfulframe_detector_schedule_next_tick(maximum_tick, 2.0f, 0.5f) == maximum_tick);
	check(faithfulframe_detector_schedule_next_tick(maximum_tick - 2, 2.0f, 0.1f) == maximum_tick);
	check(faithfulframe_detector_schedule_next_tick(maximum_tick - 30, 2.0f, 0.1f) == maximum_tick - 10);
	return 0;
}
