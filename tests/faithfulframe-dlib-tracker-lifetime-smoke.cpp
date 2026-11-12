#include <obs-module.h>
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
	auto *tracker = new face_tracker_dlib();
	if (!tracker->start()) {
		delete tracker;
		std::fprintf(stderr, "dlib tracker lifetime smoke failed to start worker\n");
		return 1;
	}

	if (!wait_for(worker_entered, std::chrono::seconds(10))) {
		release_worker.store(true, std::memory_order_release);
		delete tracker;
		std::fprintf(stderr, "dlib tracker lifetime smoke failed to enter worker\n");
		return 1;
	}

	std::atomic<bool> destroy_complete{false};
	std::thread destroyer([&] {
		delete tracker;
		destroy_complete.store(true, std::memory_order_release);
	});

	// The worker hook holds face_tracker_base's mutex. Reaching stop() without
	// completing destruction proves teardown is waiting for active worker work.
	const auto stop_reached = wait_for(stop_attempted, std::chrono::seconds(10));
	const bool destruction_waited = stop_reached && !destroy_complete.load(std::memory_order_acquire);
	release_worker.store(true, std::memory_order_release);
	destroyer.join();

	if (!destruction_waited || !destroy_complete.load(std::memory_order_acquire)) {
		std::fprintf(stderr, "dlib tracker lifetime smoke failed to wait for worker teardown\n");
		return 1;
	}
	return 0;
}
