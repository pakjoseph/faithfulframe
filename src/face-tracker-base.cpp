#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include "plugin-macros.generated.h"
#include "face-tracker-base.h"
#ifndef _WIN32
#include <sys/time.h>
#include <sys/resource.h>
#else // _WIN32
#include <windows.h>
#endif // _WIN32

face_tracker_base::face_tracker_base()
{
	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&cond, NULL);
	stop_requested = 0;
	running = 0;
	leak_test = bmalloc(1);
}

face_tracker_base::~face_tracker_base()
{
	bfree(leak_test);
	pthread_cond_destroy(&cond);
	pthread_mutex_destroy(&mutex);
}

void *face_tracker_base::thread_routine(void *p)
{
	face_tracker_base *base = (face_tracker_base *)p;
#ifndef _WIN32
	setpriority(PRIO_PROCESS, 0, 17);
#else  // _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif // _WIN32
	os_set_thread_name("face-trk");

	base->lock();
	while (!base->stop_requested.load(std::memory_order_relaxed)) {
		if (!base->suspend_requested.load(std::memory_order_relaxed)) {
			try {
				base->track_main();
			} catch (std::exception &e) {
				blog(LOG_ERROR, "track_main: exception %s", e.what());
			} catch (...) {
				blog(LOG_ERROR, "track_main: unknown exception");
			}
		}
		pthread_cond_wait(&base->cond, &base->mutex);
	}
	base->stopped.store(true, std::memory_order_release);
	base->unlock();
	return NULL;
}

bool face_tracker_base::start()
{
	stop_requested.store(false, std::memory_order_relaxed);
	stopped.store(false, std::memory_order_relaxed);
	suspend_requested.store(false, std::memory_order_relaxed);
	if (!running) {
		blog(LOG_INFO, "face_tracker_base: starting a new thread.");
		const int result = pthread_create(&thread, NULL, thread_routine, (void *)this);
		if (result != 0) {
			stop_requested.store(true, std::memory_order_relaxed);
			stopped.store(true, std::memory_order_relaxed);
			blog(LOG_ERROR, "face_tracker_base: failed to start thread (error %d)", result);
			return false;
		}
		running = 1;
	} else {
		lock();
		signal();
		unlock();
	}
	return true;
}

void face_tracker_base::stop()
{
	lock();
	stop_requested.store(true, std::memory_order_relaxed);
	signal();
	unlock();
	if (running) {
		blog(LOG_INFO, "face_tracker_base: joining the thread...");
		pthread_join(thread, NULL);
		running = 0;
		blog(LOG_INFO, "face_tracker_base: joined the thread.");
	}
}

void face_tracker_base::request_stop()
{
	lock();
	stop_requested.store(true, std::memory_order_relaxed);
	signal();
	unlock();
}

bool face_tracker_base::is_stopped()
{
	if (stopped.load(std::memory_order_acquire)) {
		if (running) {
			blog(LOG_INFO, "face_tracker_base: joining the thread...");
			pthread_join(thread, NULL);
			running = 0;
		}
		return 1;
	} else
		return 0;
}

void face_tracker_base::request_suspend()
{
	suspend_requested.store(true, std::memory_order_relaxed);
}
