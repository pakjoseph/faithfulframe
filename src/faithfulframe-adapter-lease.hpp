#pragma once

#include <atomic>
#include <cstdint>

namespace faithfulframe {

enum class safe_adapter_availability_issue : uint32_t {
	none = 0,
	another_adapter_owns_tracking = 1,
	no_source_selected = 2,
	invalid_source_binding = 3,
	waiting_for_video_signal = 4,
	session_start_failed = 5,
	session_worker_failed_safe_wide = 6,
};

enum class safe_adapter_configuration_notice : uint32_t {
	none = 0,
	source_binding_changed = 1,
	framing_settings_changed = 2,
	tracking_settings_changed = 3,
};

/// Process-wide lease for the one OBS adapter allowed to run safe tracking.
class single_adapter_lease {
public:
	static bool try_acquire(const void *owner)
	{
		if (!owner)
			return false;
		const void *expected = nullptr;
		if (owner_slot.compare_exchange_strong(expected, owner, std::memory_order_acq_rel,
						       std::memory_order_acquire))
			return true;
		return expected == owner;
	}

	static void release(const void *owner)
	{
		if (!owner)
			return;
		const void *expected = owner;
		owner_slot.compare_exchange_strong(expected, nullptr, std::memory_order_release,
						   std::memory_order_relaxed);
	}

	static bool owns(const void *owner)
	{
		if (!owner)
			return false;
		return owner_slot.load(std::memory_order_acquire) == owner;
	}

private:
	inline static std::atomic<const void *> owner_slot{nullptr};
};

static_assert(std::atomic<const void *>::is_always_lock_free,
	      "safe-adapter lease operations must not block OBS callbacks");

} // namespace faithfulframe
