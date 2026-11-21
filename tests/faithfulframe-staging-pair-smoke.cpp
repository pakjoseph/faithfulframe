#include "faithfulframe-staging-pair.hpp"

#include <cmath>
#include <limits>

namespace {
bool first_frame_is_staged_without_mapping()
{
	faithfulframe_staging_pair pair{};
	faithfulframe_staging_pair_plan plan{};
	if (!pair.prepare(3840, 2160, 2.0f, 100, 1, 100, 500, plan))
		return false;
	return !plan.has_previous && !plan.reset_surfaces && plan.stage_slot == 0;
}

bool next_frame_maps_the_previous_slot()
{
	faithfulframe_staging_pair pair{};
	faithfulframe_staging_pair_plan plan{};
	if (!pair.prepare(3840, 2160, 2.0f, 100, 7, 100, 500, plan) || plan.has_previous)
		return false;
	if (!pair.prepare(3840, 2160, 2.0f, 116, 7, 116, 500, plan))
		return false;
	if (!plan.has_previous || plan.reset_surfaces || plan.stage_slot != 1 || plan.previous_slot != 0 ||
	    plan.previous.source_width != 3840 || plan.previous.source_height != 2160 || plan.previous.scale != 2.0f ||
	    plan.previous.captured_at_ms != 100 || plan.previous.detection_epoch != 7)
		return false;
	pair.finish_mapping(plan.previous_slot);
	if (!pair.prepare(3840, 2160, 2.0f, 132, 7, 132, 500, plan))
		return false;
	return plan.has_previous && plan.stage_slot == 0 && plan.previous_slot == 1 &&
	       plan.previous.captured_at_ms == 116;
}

bool geometry_and_epoch_changes_drop_pending_frames()
{
	faithfulframe_staging_pair pair{};
	faithfulframe_staging_pair_plan plan{};
	if (!pair.prepare(3840, 2160, 2.0f, 100, 1, 100, 500, plan) ||
	    !pair.prepare(3840, 2160, 2.0f, 116, 1, 116, 500, plan))
		return false;
	if (!pair.prepare(1920, 1080, 2.0f, 132, 1, 132, 500, plan) || plan.has_previous || !plan.reset_surfaces ||
	    plan.stage_slot != 0)
		return false;
	if (!pair.prepare(1920, 1080, 2.0f, 148, 2, 148, 500, plan) || plan.has_previous || plan.reset_surfaces ||
	    plan.stage_slot != 0)
		return false;
	return pair.prepare(1920, 1080, 2.0f, 164, 2, 164, 500, plan) && plan.has_previous &&
	       plan.previous.detection_epoch == 2 && plan.previous.captured_at_ms == 148;
}

bool stale_or_failed_staging_data_is_never_reused()
{
	faithfulframe_staging_pair pair{};
	faithfulframe_staging_pair_plan plan{};
	if (!pair.prepare(1920, 1080, 1.0f, 100, 1, 100, 500, plan) ||
	    !pair.prepare(1920, 1080, 1.0f, 700, 1, 700, 500, plan) || plan.has_previous)
		return false;
	if (!pair.prepare(1920, 1080, 1.0f, 716, 1, 716, 500, plan) || !plan.has_previous)
		return false;
	pair.finish_mapping(plan.previous_slot);
	if (!pair.prepare(1920, 1080, 1.0f, 732, 1, 732, 500, plan) || !plan.has_previous)
		return false;
	pair.finish_mapping(plan.previous_slot);
	if (!pair.prepare(1920, 1080, 2.0f, 748, 1, 748, 500, plan) || plan.has_previous || !plan.reset_surfaces)
		return false;
	pair.abandon_stage(plan.stage_slot);
	return pair.prepare(1920, 1080, 2.0f, 764, 1, 764, 500, plan) && !plan.has_previous;
}

bool invalid_inputs_leave_the_pair_unchanged()
{
	faithfulframe_staging_pair pair{};
	faithfulframe_staging_pair_plan plan{};
	if (!pair.prepare(1920, 1080, 1.0f, 100, 1, 100, 500, plan))
		return false;
	const auto next_slot = pair.next_stage_slot;
	if (pair.prepare(0, 1080, 1.0f, 116, 1, 116, 500, plan) ||
	    pair.prepare(1920, 1080, std::numeric_limits<float>::quiet_NaN(), 116, 1, 116, 500, plan) ||
	    pair.prepare(1920, 1080, 1.0f, 116, 0, 116, 500, plan) ||
	    pair.prepare(1920, 1080, 1.0f, 116, 1, 115, 500, plan))
		return false;
	if (pair.next_stage_slot != next_slot)
		return false;
	return pair.prepare(1920, 1080, 1.0f, 116, 1, 116, 500, plan) && plan.has_previous &&
	       plan.previous.captured_at_ms == 100;
}
} // namespace

int main()
{
	if (!first_frame_is_staged_without_mapping())
		return 1;
	if (!next_frame_maps_the_previous_slot())
		return 2;
	if (!geometry_and_epoch_changes_drop_pending_frames())
		return 3;
	if (!stale_or_failed_staging_data_is_never_reused())
		return 4;
	if (!invalid_inputs_leave_the_pair_unchanged())
		return 5;
	return 0;
}
