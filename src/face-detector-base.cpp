#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include "plugin-macros.generated.h"
#include "face-detector-base.h"
#ifndef _WIN32
#include <sys/time.h>
#include <sys/resource.h>
#else // _WIN32
#include <windows.h>
#endif // _WIN32

face_detector_base::face_detector_base()
{
	mutex_initialized = false;
	cond_initialized = false;
	request_stop = 0;
	running = 0;
	work_pending = false;
	leak_test = bmalloc(1);
	const int mutex_result = pthread_mutex_init(&mutex, NULL);
	if (mutex_result != 0) {
		blog(LOG_ERROR, "face_detector_base: failed to initialize mutex (error %d)", mutex_result);
		return;
	}
	mutex_initialized = true;
	const int cond_result = pthread_cond_init(&cond, NULL);
	if (cond_result != 0)
		blog(LOG_ERROR, "face_detector_base: failed to initialize condition variable (error %d)", cond_result);
	else
		cond_initialized = true;
}

face_detector_base::~face_detector_base()
{
	bfree(leak_test);
	if (cond_initialized)
		pthread_cond_destroy(&cond);
	if (mutex_initialized)
		pthread_mutex_destroy(&mutex);
}

void *face_detector_base::thread_routine(void *p)
{
	face_detector_base *base = (face_detector_base *)p;
#ifndef _WIN32
	setpriority(PRIO_PROCESS, 0, 19);
#else  // _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif // _WIN32
	os_set_thread_name("face-det");

	base->lock();
	while (!base->request_stop) {
		while (!base->request_stop && !base->work_pending)
			pthread_cond_wait(&base->cond, &base->mutex);
		if (base->request_stop)
			break;
		base->work_pending = false;
		const uint64_t started_ns = os_gettime_ns();
		try {
			base->detect_main();
		} catch (std::exception &e) {
			blog(LOG_ERROR, "detect_main: exception %s", e.what());
		} catch (...) {
			blog(LOG_ERROR, "detect_main: unknown exception");
		}
		const uint64_t finished_ns = os_gettime_ns();
		const uint64_t elapsed_us = finished_ns >= started_ns ? (finished_ns - started_ns) / 1000 : 0;
		base->last_work_us.store(elapsed_us, std::memory_order_relaxed);
		uint64_t previous_max = base->max_work_us.load(std::memory_order_relaxed);
		while (previous_max < elapsed_us &&
		       !base->max_work_us.compare_exchange_weak(previous_max, elapsed_us, std::memory_order_relaxed,
								std::memory_order_relaxed)) {
		}
		base->completed_work.fetch_add(1, std::memory_order_relaxed);
	}
	base->unlock();
	return NULL;
}

bool face_detector_base::start()
{
	if (!mutex_initialized || !cond_initialized)
		return false;
	lock();
	blog(LOG_INFO, "face_detector_base: starting the thread.");
	request_stop = 0;
	work_pending = false;
	unlock();
	const int result = pthread_create(&thread, NULL, thread_routine, (void *)this);
	if (result != 0) {
		blog(LOG_ERROR, "face_detector_base: failed to start thread (error %d)", result);
		return false;
	}
	running = 1;
	return true;
}

void face_detector_base::stop()
{
	blog(LOG_INFO, "face_detector_base: stopping the thread...");
	if (!mutex_initialized || !cond_initialized)
		return;
	lock();
	request_stop = 1;
	signal();
	unlock();
	if (running) {
		pthread_join(thread, NULL);
		running = 0;
	}
	blog(LOG_INFO, "face_detector_base: stopped the thread...");
}
