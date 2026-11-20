#pragma once

#include <cstdint>
#include <cstddef>

#include "faithfulframe_ffi.h"

/// Version of the immutable tracking snapshot used for the most recently
/// rendered monitor frame. Mouse selection must use this exact version.
struct faithfulframe_preview_render_stamp
{
	bool valid = false;
	uint64_t source_id = 0;
	uint64_t session_id = 0;
	uint64_t status_version = 0;
	uint64_t observation_version = 0;
	uint64_t source_generation = 0;
	uint64_t session_generation = 0;
	uint64_t stream_generation = 0;
	uint64_t geometry_generation = 0;
	uint64_t tracker_generation = 0;
	uint64_t candidate_preview_revision = 0;
	uint32_t display_width = 0;
	uint32_t display_height = 0;
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	ff_rect crop{0.0f, 0.0f, 1.0f, 1.0f};
	ff_rect viewport{0.0f, 0.0f, 1.0f, 1.0f};
	bool full_source = false;
};

/// Compare the ordered hitboxes that were offered for the displayed preview.
/// Expiry and tracker timestamps may advance without changing mouse geometry.
inline bool faithfulframe_preview_candidate_geometry_matches(const ff_candidate *previous, std::size_t previous_count,
							     const ff_candidate *current, std::size_t current_count)
{
	if (previous_count != current_count || (previous_count && !previous) || (current_count && !current))
		return false;
	for (std::size_t i = 0; i < current_count; ++i) {
		if (previous[i].handle != current[i].handle || previous[i].rect.x != current[i].rect.x ||
		    previous[i].rect.y != current[i].rect.y || previous[i].rect.width != current[i].rect.width ||
		    previous[i].rect.height != current[i].rect.height)
			return false;
	}
	return true;
}

inline bool faithfulframe_preview_render_stamp_matches(const faithfulframe_preview_render_stamp &rendered,
						       const faithfulframe_preview_render_stamp &current)
{
	return rendered.valid && current.valid && rendered.source_id == current.source_id &&
	       rendered.session_id == current.session_id && rendered.status_version == current.status_version &&
	       rendered.observation_version == current.observation_version &&
	       rendered.source_generation == current.source_generation &&
	       rendered.session_generation == current.session_generation &&
	       rendered.stream_generation == current.stream_generation &&
	       rendered.geometry_generation == current.geometry_generation &&
	       rendered.tracker_generation == current.tracker_generation &&
	       rendered.candidate_preview_revision == current.candidate_preview_revision &&
	       rendered.display_width == current.display_width && rendered.display_height == current.display_height &&
	       rendered.output_width == current.output_width && rendered.output_height == current.output_height &&
	       rendered.crop.x == current.crop.x && rendered.crop.y == current.crop.y &&
	       rendered.crop.width == current.crop.width && rendered.crop.height == current.crop.height &&
	       rendered.viewport.x == current.viewport.x && rendered.viewport.y == current.viewport.y &&
	       rendered.viewport.width == current.viewport.width &&
	       rendered.viewport.height == current.viewport.height && rendered.full_source == current.full_source;
}

struct obs_source;
void faithfulframe_monitor_begin_render(struct obs_source *monitor);
bool faithfulframe_monitor_get_render_stamp(struct obs_source *monitor, faithfulframe_preview_render_stamp &stamp);
