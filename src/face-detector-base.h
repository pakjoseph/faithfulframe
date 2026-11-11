#pragma once
#ifndef FAITHFULFRAME_DETECTOR_WORKER_STANDALONE
#include <obs-module.h>
#include "plugin-macros.generated.h"
#include "helper.hpp"
#else
struct rect_s;
#endif
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class face_detector_base {
	std::thread thread;
	uint64_t result_epoch = 0;
	std::mutex mutex;
	std::mutex lifecycle_mutex;
	std::condition_variable cond;
	bool request_stop;
	bool work_pending;
	std::atomic<uint64_t> last_work_us{0};
	std::atomic<uint64_t> max_work_us{0};
	std::atomic<uint64_t> completed_work{0};

	static void thread_routine(face_detector_base *);
	virtual void detect_main() = 0;

protected:
	void set_result_epoch(uint64_t epoch) { result_epoch = epoch; }

public:
	face_detector_base();
	virtual ~face_detector_base();

	int lock()
	{
		mutex.lock();
		return 0;
	}
	int trylock() { return mutex.try_lock() ? 0 : 1; }
	int unlock()
	{
		mutex.unlock();
		return 0;
	}
	/// Caller must hold mutex; a queued item is not a completed detector result.
	bool is_idle_locked() const { return !request_stop && !work_pending; }
	// Caller must hold mutex; sets the predicate as well as notifying the worker.
	int signal()
	{
		work_pending = true;
		cond.notify_one();
		return 0;
	}

	virtual void set_texture(std::shared_ptr<class texture_object> &, int crop_l, int crop_r, int crop_t,
				 int crop_b) = 0;
	virtual void get_faces(std::vector<struct rect_s> &) = 0;
	uint64_t completed_result_epoch() const { return result_epoch; }

	bool start();
	void stop();
	/// Nonblocking readiness check; false means active or queued work owns the worker.
	bool try_idle();
	uint64_t last_work_duration_us() const { return last_work_us.load(std::memory_order_relaxed); }
	uint64_t max_work_duration_us() const { return max_work_us.load(std::memory_order_relaxed); }
	uint64_t completed_work_count() const { return completed_work.load(std::memory_order_relaxed); }
};

inline bool face_detector_result_epoch_is_current(uint64_t result_epoch, uint64_t current_epoch)
{
	return current_epoch != 0 && result_epoch == current_epoch;
}
