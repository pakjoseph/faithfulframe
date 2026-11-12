#include "face-tracker-base.h"
#include <util/bmem.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" void faithfulframe_worker_log(int, const char *, ...) {}
extern "C" void *bmalloc(size_t size)
{
	return std::malloc(size);
}
extern "C" void bfree(void *pointer)
{
	std::free(pointer);
}

class throwing_tracker final : public face_tracker_base {
	const bool throw_unknown;
	std::atomic<uint32_t> calls{0};

	void track_main() override
	{
		calls.fetch_add(1, std::memory_order_relaxed);
		if (throw_unknown)
			throw 7;
		throw std::runtime_error("expected tracker failure");
	}

public:
	explicit throwing_tracker(bool throw_unknown_) : throw_unknown(throw_unknown_) {}

	uint32_t call_count() const { return calls.load(std::memory_order_relaxed); }
	void set_texture(std::shared_ptr<texture_object> &) override {}
	void set_position(const rect_s &) override {}
	void set_upsize_info(const rectf_s &) override {}
	void set_landmark_detection(const char *) override {}
	bool get_face(rect_s &, uint64_t &) override { return false; }
	bool get_landmark(std::vector<pointf_s> &) override { return false; }
};

static bool wait_for_failure_and_idle(throwing_tracker &tracker)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline) {
		if (tracker.has_failed() && tracker.trylock() == 0) {
			tracker.unlock();
			return true;
		}
		std::this_thread::yield();
	}
	return false;
}

static bool exception_faults_worker(bool throw_unknown)
{
	throwing_tracker tracker(throw_unknown);
	if (!tracker.start() || !wait_for_failure_and_idle(tracker) || tracker.call_count() != 1 || tracker.start())
		return false;
	tracker.stop();
	return tracker.call_count() == 1;
}

int main()
{
	if (!exception_faults_worker(false)) {
		std::fprintf(stderr, "standard tracker exception was not contained and quarantined\n");
		return 1;
	}
	if (!exception_faults_worker(true)) {
		std::fprintf(stderr, "unknown tracker exception was not contained and quarantined\n");
		return 2;
	}
	return 0;
}
