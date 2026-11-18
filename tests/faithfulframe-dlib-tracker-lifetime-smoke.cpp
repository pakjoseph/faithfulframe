#include <obs-module.h>
#include "faithfulframe-core-bridge.hpp"
#include "faithfulframe-worker-log.hpp"
#include "face-tracker-dlib.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" void *bmalloc(size_t size)
{
	return std::malloc(size);
}

extern "C" void bfree(void *pointer)
{
	std::free(pointer);
}

extern "C" void *bmemdup(const void *source, size_t size)
{
	void *copy = bmalloc(size);
	if (copy)
		std::memcpy(copy, source, size);
	return copy;
}

extern "C" void faithfulframe_worker_log(int, const char *, ...) {}

namespace {

using steady_clock = std::chrono::steady_clock;

std::atomic<bool> worker_entered{false};
std::atomic<bool> release_worker{false};
std::atomic<bool> stop_attempted{false};

struct deferred_tracker_teardown
{
	faithfulframe_reap_task task{};
	face_tracker_dlib *tracker = nullptr;
	std::thread::id destroy_thread;
	std::atomic<bool> callback_ran_off_destroy_thread{false};
	std::atomic<bool> cleanup_complete{false};
};

bool wait_for(const std::atomic<bool> &flag, std::chrono::seconds timeout)
{
	const auto deadline = steady_clock::now() + timeout;
	while (steady_clock::now() < deadline) {
		if (flag.load(std::memory_order_acquire))
			return true;
		std::this_thread::yield();
	}
	return flag.load(std::memory_order_acquire);
}

void delete_tracker_on_reaper(void *context)
{
	auto *teardown = static_cast<deferred_tracker_teardown *>(context);
	teardown->callback_ran_off_destroy_thread.store(std::this_thread::get_id() != teardown->destroy_thread,
							std::memory_order_release);
	delete teardown->tracker;
	teardown->tracker = nullptr;
	teardown->cleanup_complete.store(true, std::memory_order_release);
}

} // namespace

extern "C" void faithfulframe_test_tracker_work_entered(void)
{
	if (!worker_entered.exchange(true, std::memory_order_acq_rel)) {
		while (!release_worker.load(std::memory_order_acquire))
			std::this_thread::yield();
	}
}

extern "C" void faithfulframe_test_tracker_stop_attempted(void)
{
	stop_attempted.store(true, std::memory_order_release);
}

int main()
{
	if (faithfulframe_executor_reaper_start() != 1 || faithfulframe_teardown_register_instance() != 1) {
		std::fprintf(stderr, "dlib tracker lifetime smoke failed to start the module reaper\n");
		return 1;
	}
	faithfulframe_teardown_instance_created();

	auto *tracker = new face_tracker_dlib();
	if (!tracker->start()) {
		delete tracker;
		faithfulframe_teardown_instance_destroyed();
		faithfulframe_teardown_begin_module_unload();
		std::fprintf(stderr, "dlib tracker lifetime smoke failed to start worker\n");
		return 1;
	}

	if (!wait_for(worker_entered, std::chrono::seconds(10))) {
		release_worker.store(true, std::memory_order_release);
		delete tracker;
		faithfulframe_teardown_instance_destroyed();
		faithfulframe_teardown_begin_module_unload();
		std::fprintf(stderr, "dlib tracker lifetime smoke failed to enter worker\n");
		return 1;
	}

	deferred_tracker_teardown teardown;
	teardown.tracker = tracker;
	std::atomic<bool> destroy_callback_returned{false};
	std::atomic<bool> handoff_succeeded{false};
	std::thread destroyer([&] {
		teardown.destroy_thread = std::this_thread::get_id();
		const bool handed_off =
			faithfulframe_defer_reap_task(&teardown.task, delete_tracker_on_reaper, &teardown, true);
		handoff_succeeded.store(handed_off, std::memory_order_release);
		if (handed_off)
			faithfulframe_activate_reap_task(&teardown.task);
		else
			delete_tracker_on_reaper(&teardown);
		faithfulframe_teardown_instance_destroyed();
		destroy_callback_returned.store(true, std::memory_order_release);
	});

	// The worker hook holds face_tracker_base's mutex. The reaper should reach
	// stop() and wait there while the simulated OBS destroy callback returns.
	const auto stop_reached = wait_for(stop_attempted, std::chrono::seconds(10));
	const bool destroy_returned_while_worker_blocked =
		stop_reached && wait_for(destroy_callback_returned, std::chrono::seconds(2)) &&
		!teardown.cleanup_complete.load(std::memory_order_acquire);
	const bool cleanup_ran_off_destroy_thread =
		teardown.callback_ran_off_destroy_thread.load(std::memory_order_acquire);
	release_worker.store(true, std::memory_order_release);
	destroyer.join();
	faithfulframe_teardown_begin_module_unload();

	if (!handoff_succeeded.load(std::memory_order_acquire) || !destroy_returned_while_worker_blocked ||
	    !cleanup_ran_off_destroy_thread || !teardown.cleanup_complete.load(std::memory_order_acquire) ||
	    teardown.tracker != nullptr) {
		std::fprintf(
			stderr,
			"dlib tracker lifetime smoke failed to defer worker teardown (handoff=%d, destroy_returned=%d, "
			"off_thread=%d, cleanup=%d)\n",
			handoff_succeeded.load(std::memory_order_acquire), destroy_returned_while_worker_blocked,
			cleanup_ran_off_destroy_thread, teardown.cleanup_complete.load(std::memory_order_acquire));
		return 1;
	}
	return 0;
}
