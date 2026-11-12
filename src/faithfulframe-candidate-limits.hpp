#pragma once

#include <cstddef>

static constexpr std::size_t faithfulframe_max_candidates = 8;

// Candidate trackers include the in-flight detector placeholder and retained
// idle workers. Keep their combined count bounded even when workers are still
// finishing after retirement.
inline bool faithfulframe_tracker_worker_budget_available(std::size_t active, std::size_t idle)
{
	return active < faithfulframe_max_candidates && idle < faithfulframe_max_candidates - active;
}
