#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

static constexpr std::size_t faithfulframe_max_candidates = 8;

// Candidate handles travel through OBS's signed integer fields. Exhaust the
// allocator rather than wrapping or reusing an identity that may still be
// present in a preview payload or delayed command.
inline std::uint64_t faithfulframe_allocate_candidate_handle(std::uint64_t &next_candidate_handle)
{
	const std::uint64_t maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	if (next_candidate_handle == 0 || next_candidate_handle > maximum)
		return 0;
	const std::uint64_t handle = next_candidate_handle;
	next_candidate_handle = handle == maximum ? 0 : handle + 1;
	return handle;
}

// Candidate trackers include the in-flight detector placeholder and retained
// idle workers. Keep their combined count bounded even when workers are still
// finishing after retirement.
inline bool faithfulframe_tracker_worker_budget_available(std::size_t active, std::size_t idle)
{
	return active < faithfulframe_max_candidates && idle < faithfulframe_max_candidates - active;
}
