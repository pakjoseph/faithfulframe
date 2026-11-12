#include "face-detector-base.h"
#include "faithfulframe-worker-log.hpp"
#include "faithfulframe-worker-log-gate.hpp"
#include <chrono>
#include <cstdarg>
#include <condition_variable>
#include <exception>
#include <mutex>
#ifndef FAITHFULFRAME_DETECTOR_WORKER_STANDALONE
#include <obs-module.h>
#include "plugin-macros.generated.h"
#endif
#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#ifdef FAITHFULFRAME_DETECTOR_WORKER_STANDALONE
#define detector_log(...) ((void)0)
extern "C" void faithfulframe_worker_log_start(void) {}
extern "C" void faithfulframe_worker_log_shutdown(void) {}
extern "C" void faithfulframe_worker_log(int, const char *, ...) {}
#else
namespace {
faithfulframe_worker_log_gate worker_log_gate;
} // namespace

extern "C" void faithfulframe_worker_log_start(void)
{
	worker_log_gate.start();
}

extern "C" void faithfulframe_worker_log_shutdown(void)
{
	worker_log_gate.shutdown();
}

extern "C" void faithfulframe_worker_log(int level, const char *format, ...)
{
	if (!worker_log_gate.try_begin())
		return;

	va_list args;
	va_start(args, format);
	blogva(level, format, args);
	va_end(args);
	worker_log_gate.end();
}

#define detector_log(...) faithfulframe_worker_log(__VA_ARGS__)
#endif

face_detector_base::face_detector_base() : request_stop(false), work_pending(false) {}

face_detector_base::~face_detector_base()
{
	stop();
}

void face_detector_base::thread_routine(face_detector_base *base)
{
#if defined(__APPLE__)
	// Keep CPU-bound detector work below OBS's interactive/rendering QoS.
	// Darwin's PRIO_PROCESS would instead change the whole OBS process.
	(void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif !defined(_WIN32)
	setpriority(PRIO_PROCESS, 0, 19);
#else
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
	std::unique_lock<std::mutex> lock(base->mutex);
	while (!base->request_stop) {
		while (!base->request_stop && !base->work_pending)
			base->cond.wait(lock);
		if (base->request_stop)
			break;
		base->work_pending = false;
		const auto started = std::chrono::steady_clock::now();
		bool work_failed = false;
		try {
			base->detect_main();
		} catch (std::exception &e) {
			base->failed.store(true, std::memory_order_release);
			work_failed = true;
			detector_log(LOG_ERROR, "detect_main: exception %s", e.what());
		} catch (...) {
			base->failed.store(true, std::memory_order_release);
			work_failed = true;
			detector_log(LOG_ERROR, "detect_main: unknown exception");
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
					     std::chrono::steady_clock::now() - started)
					     .count();
		const uint64_t elapsed_us = elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
		base->last_work_us.store(elapsed_us, std::memory_order_relaxed);
		uint64_t previous_max = base->max_work_us.load(std::memory_order_relaxed);
		while (previous_max < elapsed_us &&
		       !base->max_work_us.compare_exchange_weak(previous_max, elapsed_us, std::memory_order_relaxed,
								std::memory_order_relaxed)) {
		}
		base->completed_work.fetch_add(1, std::memory_order_relaxed);
		if (work_failed)
			break;
	}
}

bool face_detector_base::start()
{
	if (failed.load(std::memory_order_acquire))
		return false;
	std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
	std::lock_guard<std::mutex> lock(mutex);
	if (thread.joinable())
		return false;
	detector_log(LOG_INFO, "face_detector_base: starting the thread.");
	request_stop = false;
	work_pending = false;
	try {
		thread = std::thread(&face_detector_base::thread_routine, this);
	} catch (const std::exception &e) {
		request_stop = true;
		detector_log(LOG_ERROR, "face_detector_base: failed to start thread: %s", e.what());
		return false;
	} catch (...) {
		request_stop = true;
		detector_log(LOG_ERROR, "face_detector_base: failed to start thread");
		return false;
	}
	return true;
}

void face_detector_base::stop()
{
	std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
	detector_log(LOG_INFO, "face_detector_base: stopping the thread...");
	{
		std::lock_guard<std::mutex> lock(mutex);
		request_stop = true;
		work_pending = true;
		cond.notify_one();
	}
	if (thread.joinable())
		thread.join();
	detector_log(LOG_INFO, "face_detector_base: stopped the thread...");
}

bool face_detector_base::try_idle()
{
	if (trylock() != 0)
		return false;
	const bool idle = !request_stop && !work_pending;
	unlock();
	return idle;
}
