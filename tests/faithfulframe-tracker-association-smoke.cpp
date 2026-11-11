#include "face-tracker-association.hpp"

#include <cstdlib>
#include <cstdio>

static void check_impl(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "tracker association check failed at line %d\n", line);
		std::abort();
	}
}

#define check(condition) check_impl((condition), __LINE__)

int main()
{
	const ff_tracker_box_i first{100, 100, 200, 220};
	check(ff_tracker_boxes_match(first, first));
	check(ff_tracker_boxes_match(first, ff_tracker_box_i{110, 105, 210, 225}));
	check(ff_tracker_boxes_match(first, ff_tracker_box_i{120, 130, 170, 190}));
	check(!ff_tracker_boxes_match(first, ff_tracker_box_i{250, 100, 350, 220}));
	check(!ff_tracker_boxes_match(first, ff_tracker_box_i{0, 0, 0, 100}));
	check(!ff_tracker_boxes_match(first, ff_tracker_box_i{200, 100, 100, 220}));
	return 0;
}
