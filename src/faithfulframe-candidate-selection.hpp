#pragma once

#include <cstddef>
#include <cstdint>

#include "faithfulframe_ffi.h"

/// Finds a candidate's current list index only while its source/session binding
/// and all continuity-relevant generations still match. A numeric handle alone
/// is insufficient because handles are unique only within one session.
inline bool faithfulframe_candidate_restore_index(uint64_t previous_handle, ff_identity previous_identity,
						  ff_generations previous_generations, ff_identity current_identity,
						  ff_generations current_generations, const uint64_t *current_handles,
						  size_t current_count, size_t &index)
{
	if (!previous_handle || !current_handles || previous_identity.source != current_identity.source ||
	    previous_identity.session != current_identity.session ||
	    previous_generations.source != current_generations.source ||
	    previous_generations.session != current_generations.session ||
	    previous_generations.stream != current_generations.stream ||
	    previous_generations.geometry != current_generations.geometry ||
	    previous_generations.tracker != current_generations.tracker)
		return false;

	for (size_t i = 0; i < current_count; ++i) {
		if (current_handles[i] == previous_handle) {
			index = i;
			return true;
		}
	}
	return false;
}
