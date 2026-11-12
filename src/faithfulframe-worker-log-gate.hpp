#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

class faithfulframe_worker_log_gate {
public:
	void start()
	{
		std::lock_guard<std::mutex> lock(mutex);
		accepting = true;
	}

	bool try_begin()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!accepting)
			return false;
		++in_flight;
		return true;
	}

	void end()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (in_flight > 0 && --in_flight == 0)
			drained.notify_all();
	}

	void shutdown()
	{
		std::unique_lock<std::mutex> lock(mutex);
		accepting = false;
		drained.wait(lock, [this] { return in_flight == 0; });
	}

private:
	std::mutex mutex;
	std::condition_variable drained;
	std::size_t in_flight = 0;
	bool accepting = false;
};
