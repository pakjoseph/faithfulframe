#pragma once

#include <cmath>
#include <cstdint>

struct faithfulframe_staged_frame_metadata
{
	uint32_t source_width;
	uint32_t source_height;
	float scale;
	uint64_t captured_at_ms;
	uint64_t detection_epoch;
	bool valid;
};

struct faithfulframe_staging_pair_plan
{
	uint8_t stage_slot;
	uint8_t previous_slot;
	bool reset_surfaces;
	bool has_previous;
	faithfulframe_staged_frame_metadata previous;
};

/// Schedules one OBS staging surface for the current frame and maps the other
/// surface from the preceding frame. This gives the GPU a frame to process the
/// transfer before the CPU maps it.
struct faithfulframe_staging_pair
{
	static constexpr uint8_t slot_count = 2;

	bool configured;
	uint8_t next_stage_slot;
	uint32_t source_width;
	uint32_t source_height;
	float scale;
	uint64_t detection_epoch;
	faithfulframe_staged_frame_metadata slots[slot_count];

	void reset() noexcept
	{
		configured = false;
		next_stage_slot = 0;
		source_width = 0;
		source_height = 0;
		scale = 0.0f;
		detection_epoch = 0;
		for (auto &slot : slots)
			slot = {};
	}

	bool prepare(uint32_t width, uint32_t height, float image_scale, uint64_t captured_at_ms,
		     uint64_t current_detection_epoch, uint64_t now_ms, uint64_t max_previous_age_ms,
		     faithfulframe_staging_pair_plan &plan) noexcept
	{
		plan = {};
		if (!width || !height || !std::isfinite(image_scale) || image_scale < 1.0f ||
		    !current_detection_epoch || captured_at_ms > now_ms)
			return false;

		const bool geometry_changed =
			configured && (source_width != width || source_height != height || scale != image_scale);
		const bool epoch_changed = configured && detection_epoch != current_detection_epoch;
		if (!configured || geometry_changed || epoch_changed) {
			for (auto &slot : slots)
				slot.valid = false;
			next_stage_slot = 0;
			plan.reset_surfaces = geometry_changed;
			source_width = width;
			source_height = height;
			scale = image_scale;
			detection_epoch = current_detection_epoch;
			configured = true;
		}

		const uint8_t stage_slot = next_stage_slot;
		const uint8_t previous_slot = static_cast<uint8_t>(stage_slot ^ 1U);
		const auto &previous = slots[previous_slot];
		if (previous.valid && previous.detection_epoch == current_detection_epoch &&
		    previous.captured_at_ms <= now_ms && now_ms - previous.captured_at_ms <= max_previous_age_ms) {
			plan.has_previous = true;
			plan.previous = previous;
			plan.previous_slot = previous_slot;
		} else {
			slots[previous_slot].valid = false;
		}

		plan.stage_slot = stage_slot;
		slots[stage_slot] = {width, height, image_scale, captured_at_ms, current_detection_epoch, true};
		next_stage_slot = previous_slot;
		return true;
	}

	void finish_mapping(uint8_t slot) noexcept
	{
		if (slot < slot_count)
			slots[slot].valid = false;
	}

	void abandon_stage(uint8_t slot) noexcept
	{
		if (slot < slot_count)
			slots[slot].valid = false;
	}
};
