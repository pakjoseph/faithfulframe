#include "face-tracker-association.hpp"

#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <limits>

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
	check(ff_tracker_score_is_usable(0.001f));
	check(!ff_tracker_score_is_usable(0.0f));
	check(!ff_tracker_score_is_usable(-0.001f));
	check(!ff_tracker_score_is_usable(std::numeric_limits<float>::infinity()));
	check(!ff_tracker_score_is_usable(-std::numeric_limits<float>::infinity()));
	check(!ff_tracker_score_is_usable(std::numeric_limits<float>::quiet_NaN()));
	check(ff_tracker_confidence_from_psr(20.0, 20.0) == 1.0f);
	check(std::abs(ff_tracker_confidence_from_psr(10.0, 20.0) - 0.5f) < 1e-6f);
	check(ff_tracker_confidence_from_psr(0.0, 20.0) == 0.0f);
	check(ff_tracker_confidence_from_psr(-1.0, 20.0) == 0.0f);
	check(ff_tracker_confidence_from_psr(10.0, 0.0) == 0.0f);
	check(ff_tracker_confidence_from_psr(std::numeric_limits<double>::infinity(), 20.0) == 0.0f);
	check(ff_tracker_confidence_from_psr(21.0, 20.0) == 0.0f);
	check(ff_tracker_confidence_from_psr(10.0, std::numeric_limits<double>::quiet_NaN()) == 0.0f);
	check(ff_tracker_attention_after_detection(0.2f, 0.1f, true) == 1.0f);
	const float decayed_attention = ff_tracker_attention_after_detection(0.2f, 0.1f, false);
	check(std::abs(decayed_attention - 0.2f * std::pow(0.1f, 0.1f)) < 1e-6f);
	check(ff_tracker_attention_after_detection(0.2f, 0.0f, false) > 0.0f);
	check(ff_tracker_attention_after_detection(std::numeric_limits<float>::quiet_NaN(), 0.5f, true) == 0.0f);
	check(ff_tracker_attention_after_detection(0.2f, std::numeric_limits<float>::infinity(), false) == 0.0f);

	const ff_tracker_box_i first{100, 100, 200, 220};
	check(ff_tracker_boxes_match(first, first));
	check(ff_tracker_boxes_match(first, ff_tracker_box_i{110, 105, 210, 225}));
	check(ff_tracker_boxes_match(first, ff_tracker_box_i{120, 130, 170, 190}));
	check(!ff_tracker_boxes_match(first, ff_tracker_box_i{250, 100, 350, 220}));
	check(!ff_tracker_boxes_match(first, ff_tracker_box_i{0, 0, 0, 100}));
	check(!ff_tracker_boxes_match(first, ff_tracker_box_i{200, 100, 100, 220}));
	return 0;
}
