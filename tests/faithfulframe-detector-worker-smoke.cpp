#include "face-detector-base.h"

#include <chrono>
#include <cstdio>
#include <thread>

class probe_detector final : public face_detector_base {
	void detect_main() override { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }

public:
	void set_texture(std::shared_ptr<texture_object> &, int, int, int, int) override {}
	void get_faces(std::vector<rect_s> &) override {}
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
	probe_detector detector;
	report_stage("starting worker");
	if (!detector.start()) {
		std::fprintf(stderr, "detector worker did not start\n");
		return 1;
	}
	report_stage("worker started");
	int result = 0;

	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	report_stage("initial idle check");
	if (detector.completed_work_count() != 0) {
		std::fprintf(stderr, "detector worker ran before receiving work\n");
		result = 1;
	} else {
		report_stage("signaling first work item");
		signal_work(detector);
		if (!wait_for_runs(detector, 1) || detector.last_work_duration_us() < 3'000) {
			std::fprintf(stderr, "detector work was lost or not timed\n");
			result = 2;
		} else {
			report_stage("first work item completed");
			report_stage("signaling second work item");
			signal_work(detector);
			if (!wait_for_runs(detector, 2) || detector.last_work_duration_us() < 3'000 ||
			    detector.max_work_duration_us() < detector.last_work_duration_us()) {
				std::fprintf(stderr, "detector timing did not update consistently\n");
				result = 3;
			} else {
				report_stage("second work item completed");
			}
		}
	}

	report_stage("stopping worker");
	detector.stop();
	report_stage("worker stopped");
	return result;
}
