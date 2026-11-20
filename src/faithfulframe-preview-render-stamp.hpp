#pragma once

#include <cstdint>

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
	uint32_t display_width = 0;
	uint32_t display_height = 0;
	bool full_source = false;
};

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
	       rendered.display_width == current.display_width && rendered.display_height == current.display_height &&
	       rendered.full_source == current.full_source;
}

struct obs_source;
void faithfulframe_monitor_begin_render(struct obs_source *monitor);
bool faithfulframe_monitor_get_render_stamp(struct obs_source *monitor, faithfulframe_preview_render_stamp &stamp);
