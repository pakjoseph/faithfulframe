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

	bool start();
	void stop();
	/// Nonblocking readiness check; false means active or queued work owns the worker.
	bool try_idle();
	uint64_t last_work_duration_us() const { return last_work_us.load(std::memory_order_relaxed); }
	uint64_t max_work_duration_us() const { return max_work_us.load(std::memory_order_relaxed); }
	uint64_t completed_work_count() const { return completed_work.load(std::memory_order_relaxed); }
};
