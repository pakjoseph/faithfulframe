#include "face-detector-base.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

struct probe_lifetime_state
{
	std::atomic<bool> resource_available{true};
	std::atomic<bool> accessed_after_release{false};
};

class probe_detector final : public face_detector_base {
	std::shared_ptr<probe_lifetime_state> lifetime_state;
	std::mutex detection_mutex;
	std::condition_variable detection_condition;
	std::atomic<uint64_t> queued_result_epoch{0};
	bool detecting = false;
	bool hold_detection = false;
	bool release_detection = false;

	void detect_main() override
	{
		// Model the detector retaining the generation of its captured frame while
		// inference is in progress.
		set_result_epoch(queued_result_epoch.exchange(0, std::memory_order_acq_rel));
		bool should_hold = false;
		{
			std::lock_guard<std::mutex> lock(detection_mutex);
			detecting = true;
			should_hold = hold_detection;
		}
		detection_condition.notify_all();
		if (should_hold) {
			std::unique_lock<std::mutex> lock(detection_mutex);
			detection_condition.wait(lock, [this] { return release_detection; });
			hold_detection = false;
			release_detection = false;
		} else {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		if (!lifetime_state->resource_available.load(std::memory_order_acquire))
			lifetime_state->accessed_after_release.store(true, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(detection_mutex);
			detecting = false;
		}
	}

public:
	explicit probe_detector(std::shared_ptr<probe_lifetime_state> state) : lifetime_state(std::move(state)) {}
	~probe_detector() override
	{
		stop();
		lifetime_state->resource_available.store(false, std::memory_order_release);
	}

	void set_texture(std::shared_ptr<texture_object> &, int, int, int, int) override {}
	void get_faces(std::vector<rect_s> &) override {}
	bool wait_for_detection()
	{
		std::unique_lock<std::mutex> lock(detection_mutex);
		return detection_condition.wait_for(lock, std::chrono::seconds(2), [this] { return detecting; });
	}
	void hold_next_detection()
	{
		std::lock_guard<std::mutex> lock(detection_mutex);
		detecting = false;
		hold_detection = true;
		release_detection = false;
	}
	void queue_result_epoch(uint64_t epoch) { queued_result_epoch.store(epoch, std::memory_order_release); }
	void release_held_detection()
	{
		// Preserve the existing minimum-duration timing assertion for this work.
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		{
			std::lock_guard<std::mutex> lock(detection_mutex);
			release_detection = true;
		}
		detection_condition.notify_all();
	}
};

static bool wait_for_runs(const probe_detector &detector, uint32_t expected)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline) {
		if (detector.completed_work_count() >= expected && detector.last_work_duration_us() >= 3'000)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

static bool wait_until_idle(probe_detector &detector)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline) {
		if (detector.try_idle())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

static void signal_work(probe_detector &detector)
{
	detector.lock();
	detector.signal();
	detector.unlock();
}

static void report_stage(const char *stage)
{
	std::fprintf(stderr, "detector worker smoke: %s\n", stage);
	std::fflush(stderr);
}

int main()
{
	if (!face_detector_result_epoch_is_current(4, 4) || face_detector_result_epoch_is_current(3, 4) ||
	    face_detector_result_epoch_is_current(0, 0))
		return 13;

	probe_detector detector(std::make_shared<probe_lifetime_state>());
	report_stage("starting worker");
	if (!detector.start()) {
		std::fprintf(stderr, "detector worker did not start\n");
		return 1;
	}
	report_stage("worker started");
	int result = 0;
	if (detector.start()) {
		std::fprintf(stderr, "detector worker accepted a duplicate start\n");
		result = 1;
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	report_stage("initial idle check");
	if (detector.completed_work_count() != 0) {
		std::fprintf(stderr, "detector worker ran before receiving work\n");
		result = 1;
	} else if (!detector.try_idle()) {
		std::fprintf(stderr, "idle detector was reported busy\n");
		result = 1;
	} else {
		report_stage("signaling first work item");
		detector.hold_next_detection();
		detector.queue_result_epoch(4);
		detector.lock();
		detector.signal();
		const bool pending_reported_idle = detector.is_idle_locked();
		detector.unlock();
		if (pending_reported_idle) {
			std::fprintf(stderr, "queued detector work was reported idle before the worker started\n");
			result = 11;
		}
		const bool detection_started = detector.wait_for_detection();
		if (!detection_started) {
			std::fprintf(stderr, "held detector work did not begin\n");
			result = 2;
		} else if (detector.try_idle()) {
			std::fprintf(stderr, "active detector work was reported idle\n");
			result = 3;
		}
		detector.release_held_detection();
		if (detection_started) {
			if (!wait_for_runs(detector, 1) || detector.last_work_duration_us() < 3'000) {
				std::fprintf(stderr, "detector work was lost or not timed\n");
				result = 4;
			} else {
				detector.lock();
				const uint64_t completed_epoch = detector.completed_result_epoch();
				const bool stale_epoch_accepted =
					face_detector_result_epoch_is_current(completed_epoch, 5);
				detector.unlock();
				if (completed_epoch != 4 || stale_epoch_accepted) {
					std::fprintf(
						stderr,
						"completed detector result was not tagged and rejected as stale\n");
					result = 10;
				}
			}
			if (result == 0 && !wait_until_idle(detector)) {
				std::fprintf(stderr, "completed detector was not reported idle\n");
				result = 5;
			}
			report_stage("first work item completed");
			report_stage("signaling second work item");
			signal_work(detector);
			if (!wait_for_runs(detector, 2) || detector.last_work_duration_us() < 3'000 ||
			    detector.max_work_duration_us() < detector.last_work_duration_us()) {
				std::fprintf(stderr, "detector timing did not update consistently\n");
				result = 6;
			} else {
				report_stage("second work item completed");
			}
		}
	}

	report_stage("stopping worker");
	detector.stop();
	report_stage("worker stopped");
	detector.stop();
	if (result == 0) {
		report_stage("restarting worker");
		if (!detector.start()) {
			std::fprintf(stderr, "detector worker did not restart after join\n");
			result = 7;
		} else {
			signal_work(detector);
			if (!detector.wait_for_detection()) {
				std::fprintf(stderr, "restarted detector worker did not begin work\n");
				result = 8;
			}
			detector.stop();
			const uint64_t completed_at_stop = detector.completed_work_count();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			if (completed_at_stop < 3 || detector.completed_work_count() != completed_at_stop) {
				std::fprintf(stderr, "detector worker did not drain active work before stopping\n");
				result = 9;
			}
		}
	}

	report_stage("checking derived-state teardown");
	auto lifetime_state = std::make_shared<probe_lifetime_state>();
	{
		probe_detector teardown_detector(lifetime_state);
		if (!teardown_detector.start()) {
			std::fprintf(stderr, "teardown detector did not start\n");
			result = 10;
		} else {
			signal_work(teardown_detector);
			if (!teardown_detector.wait_for_detection()) {
				std::fprintf(stderr, "teardown detector did not begin active work\n");
				result = 11;
			}
		}
	}
	if (lifetime_state->resource_available.load(std::memory_order_acquire) ||
	    lifetime_state->accessed_after_release.load(std::memory_order_acquire)) {
		std::fprintf(stderr, "derived resources were released before active detection stopped\n");
		result = 12;
	}
	return result;
}
