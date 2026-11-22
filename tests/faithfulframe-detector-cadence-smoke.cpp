#include "faithfulframe-detector-cadence.hpp"

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
	check(faithfulframe_detector_schedule_next_tick(0, 2.0f, 0.5f) == 4);
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
	check(faithfulframe_detector_schedule_next_tick(7, 2.0f, smallest_positive) == std::numeric_limits<int>::max());
	check(faithfulframe_detector_schedule_next_tick(std::numeric_limits<int>::max(), 2.0f, 0.5f) ==
	      std::numeric_limits<int>::max());
	check(faithfulframe_detector_schedule_next_tick(std::numeric_limits<int>::max() - 2, 2.0f, 0.1f) ==
	      std::numeric_limits<int>::max());
	return 0;
}
