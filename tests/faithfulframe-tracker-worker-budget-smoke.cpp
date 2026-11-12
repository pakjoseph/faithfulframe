#include "faithfulframe-candidate-limits.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

static void check_impl(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "tracker worker budget check failed at line %d\n", line);
		std::abort();
	}
}

#define check(condition) check_impl((condition), __LINE__)

int main()
{
	check(faithfulframe_tracker_worker_budget_available(0, 0));
	check(faithfulframe_tracker_worker_budget_available(7, 0));
	check(faithfulframe_tracker_worker_budget_available(3, 4));
	check(!faithfulframe_tracker_worker_budget_available(8, 0));
	check(!faithfulframe_tracker_worker_budget_available(0, 8));
	check(!faithfulframe_tracker_worker_budget_available(7, 1));
	check(!faithfulframe_tracker_worker_budget_available(std::numeric_limits<std::size_t>::max(), 0));
	check(!faithfulframe_tracker_worker_budget_available(0, std::numeric_limits<std::size_t>::max()));
	return 0;
}
