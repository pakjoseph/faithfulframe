#include "faithfulframe-worker-log-gate.hpp"

#include <atomic>
#include <chrono>
#include <thread>

int main()
{
	faithfulframe_worker_log_gate gate;
	gate.start();
	if (!gate.try_begin())
		return 1;

	std::atomic<bool> shutdown_returned{false};
	std::thread shutdown([&] {
		gate.shutdown();
		shutdown_returned.store(true, std::memory_order_release);
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (gate.try_begin()) {
		gate.end();
		if (std::chrono::steady_clock::now() >= deadline) {
			gate.end();
			shutdown.join();
			return 2;
		}
		std::this_thread::yield();
	}
	if (shutdown_returned.load(std::memory_order_acquire)) {
		gate.end();
		shutdown.join();
		return 3;
	}

	gate.end();
	shutdown.join();
	if (!shutdown_returned.load(std::memory_order_acquire) || gate.try_begin())
		return 4;

	gate.start();
	if (!gate.try_begin())
		return 5;
	gate.end();
	gate.shutdown();
	return 0;
}
