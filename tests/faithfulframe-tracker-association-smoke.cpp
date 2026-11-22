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

	const ff_tracker_box_i separated_trackers[] = {{0, 0, 100, 100}, {200, 0, 300, 100}};
	std::size_t match_counts[2]{};
	bool ambiguous[2]{};
	check(ff_tracker_record_detector_association(separated_trackers, 2, ff_tracker_box_i{10, 10, 90, 90},
						     match_counts, ambiguous));
	check(match_counts[0] == 1 && !ambiguous[0]);
	check(match_counts[1] == 0 && !ambiguous[1]);
	check(ff_tracker_record_detector_association(separated_trackers, 2, separated_trackers[1], match_counts,
						     ambiguous));
	check(match_counts[1] == 1 && !ambiguous[1]); // Independent detections preserve both identities.

	// These adjacent trackers do not overlap each other, so pairwise tracker
	// retirement cannot catch that one detector box supports both identities.
	const ff_tracker_box_i adjacent_trackers[] = {{0, 0, 100, 100}, {100, 0, 200, 100}};
	std::size_t shared_match_counts[2]{};
	bool shared_ambiguous[2]{};
	check(ff_tracker_record_detector_association(adjacent_trackers, 2, ff_tracker_box_i{50, 0, 150, 100},
						     shared_match_counts, shared_ambiguous));
	check(shared_match_counts[0] == 1 && shared_ambiguous[0]);
	check(shared_match_counts[1] == 1 && shared_ambiguous[1]);

	const ff_tracker_box_i one_tracker[] = {{100, 100, 200, 200}};
	std::size_t duplicate_detection_count[1]{};
	bool duplicate_detection_ambiguous[1]{};
	check(ff_tracker_record_detector_association(one_tracker, 1, ff_tracker_box_i{50, 100, 150, 200},
						     duplicate_detection_count, duplicate_detection_ambiguous));
	check(ff_tracker_record_detector_association(one_tracker, 1, ff_tracker_box_i{150, 100, 250, 200},
						     duplicate_detection_count, duplicate_detection_ambiguous));
	check(duplicate_detection_count[0] == 2 && duplicate_detection_ambiguous[0]);
	check(!ff_tracker_record_detector_association(nullptr, 1, first, duplicate_detection_count,
						      duplicate_detection_ambiguous));
	return 0;
}
