#pragma once

#include <cstdint>
#include <mutex>

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

/// Process-wide lease for the one OBS adapter allowed to run safe tracking.
class single_adapter_lease {
public:
	static bool try_acquire(const void *owner)
	{
		if (!owner)
			return false;
		std::lock_guard<std::mutex> lock(mutex());
		const void *&current_owner = owner_slot();
		if (current_owner && current_owner != owner)
			return false;
		current_owner = owner;
		return true;
	}

	static void release(const void *owner)
	{
		if (!owner)
			return;
		std::lock_guard<std::mutex> lock(mutex());
		const void *&current_owner = owner_slot();
		if (current_owner == owner)
			current_owner = nullptr;
	}

	static bool owns(const void *owner)
	{
		if (!owner)
			return false;
		std::lock_guard<std::mutex> lock(mutex());
		return owner_slot() == owner;
	}

private:
	static std::mutex &mutex()
	{
		static std::mutex instance;
		return instance;
	}

	static const void *&owner_slot()
	{
		static const void *owner = nullptr;
		return owner;
	}
};

} // namespace faithfulframe
