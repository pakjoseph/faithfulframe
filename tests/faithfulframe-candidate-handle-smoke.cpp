#include "faithfulframe-candidate-limits.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>

static void check_impl(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "candidate handle check failed at line %d\n", line);
		std::abort();
	}
}

#define check(condition) check_impl((condition), __LINE__)

int main()
{
	std::uint64_t next = 1;
	check(faithfulframe_allocate_candidate_handle(next) == 1);
	check(faithfulframe_allocate_candidate_handle(next) == 2);
	check(faithfulframe_allocate_candidate_handle(next) == 3);

	const std::uint64_t maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	next = maximum - 1;
	check(faithfulframe_allocate_candidate_handle(next) == maximum - 1);
	check(faithfulframe_allocate_candidate_handle(next) == maximum);
	check(next == 0);
	check(faithfulframe_allocate_candidate_handle(next) == 0);
	check(next == 0);

	// Reject invalid allocator state without wrapping to a handle already used.
	next = maximum + 1;
	check(faithfulframe_allocate_candidate_handle(next) == 0);
	check(next == maximum + 1);

	check(faithfulframe_detector_confirmation_max_age_ms(1000) == 1500);
	check(faithfulframe_detector_confirmation_max_age_ms(1500) == 2000);
	check(faithfulframe_detector_confirmation_max_age_ms(2000) == 2500);
	check(faithfulframe_detector_confirmation_max_age_ms(static_cast<uint64_t>(maximum) - 499) ==
	      static_cast<uint64_t>(maximum) + 1);
	const uint64_t maximum_uint64 = std::numeric_limits<uint64_t>::max();
	check(faithfulframe_detector_confirmation_max_age_ms(maximum_uint64 - 499) == maximum_uint64);
	check(faithfulframe_detector_confirmation_max_age_ms(maximum_uint64) == maximum_uint64);

	check(faithfulframe_detector_confirmation_is_fresh(1000, 3500, 2500));
	check(!faithfulframe_detector_confirmation_is_fresh(1000, 3501, 2500));
	check(!faithfulframe_detector_confirmation_is_fresh(3501, 3500, 2500));
	check(!faithfulframe_detector_confirmation_is_fresh(0, 0, 2500));
	return 0;
}
