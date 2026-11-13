#include "faithfulframe-candidate-limits.hpp"

#include <cstdio>
#include <cstdlib>
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
	uint64_t next = 1;
	check(faithfulframe_allocate_candidate_handle(next) == 1);
	check(faithfulframe_allocate_candidate_handle(next) == 2);
	check(faithfulframe_allocate_candidate_handle(next) == 3);

	const uint64_t maximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
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
	return 0;
}
