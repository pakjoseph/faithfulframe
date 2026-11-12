#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>

namespace faithfulframe {

/// Caller must hold the mutex that protects the associated configuration fields.
inline uint64_t advance_configuration_revision_locked(std::atomic<uint64_t> &version,
						      std::atomic<bool> &reconfiguration_pending)
{
	reconfiguration_pending.store(true, std::memory_order_release);
	const uint64_t current = version.load(std::memory_order_relaxed);
	if (current == std::numeric_limits<uint64_t>::max())
		return current;
	const uint64_t next = current + 1;
	version.store(next, std::memory_order_release);
	return next;
}

/// Clears the guard only while holding the same mutex used to publish revisions.
inline bool clear_reconfiguration_if_current(std::mutex &configuration_mutex, const std::atomic<uint64_t> &version,
					     std::atomic<bool> &reconfiguration_pending, uint64_t expected_revision)
{
	std::lock_guard<std::mutex> lock(configuration_mutex);
	if (expected_revision == std::numeric_limits<uint64_t>::max() ||
	    version.load(std::memory_order_acquire) != expected_revision)
		return false;
	reconfiguration_pending.store(false, std::memory_order_release);
	return true;
}

} // namespace faithfulframe
