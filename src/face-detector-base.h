#pragma once
#include <obs-module.h>
#include <util/threading.h>
#include <atomic>
#include <cstdint>
#include <vector>
#include <memory>
#include "plugin-macros.generated.h"
#include "helper.hpp"

class face_detector_base {
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool running;
	volatile bool request_stop;
	bool work_pending;
	void *leak_test;
	std::atomic<uint64_t> last_work_us{0};
	std::atomic<uint64_t> max_work_us{0};
	std::atomic<uint64_t> completed_work{0};

	static void *thread_routine(void *);
	virtual void detect_main() = 0;

public:
	face_detector_base();
	virtual ~face_detector_base();

	int lock() { return pthread_mutex_lock(&mutex); }
	int trylock() { return pthread_mutex_trylock(&mutex); }
	int unlock() { return pthread_mutex_unlock(&mutex); }
	// Caller must hold mutex; sets the predicate as well as notifying the worker.
	int signal()
	{
		work_pending = true;
		return pthread_cond_signal(&cond);
	}

	virtual void set_texture(std::shared_ptr<class texture_object> &, int crop_l, int crop_r, int crop_t,
				 int crop_b) = 0;
	virtual void get_faces(std::vector<struct rect_s> &) = 0;

	void start();
	void stop();
	uint64_t last_work_duration_us() const { return last_work_us.load(std::memory_order_relaxed); }
	uint64_t max_work_duration_us() const { return max_work_us.load(std::memory_order_relaxed); }
	uint64_t completed_work_count() const { return completed_work.load(std::memory_order_relaxed); }
};
