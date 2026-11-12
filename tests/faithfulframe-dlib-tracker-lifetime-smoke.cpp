#include <obs-module.h>
#include "faithfulframe-worker-log.hpp"
#include "face-tracker-dlib.h"
#include "texture-object.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

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

std::shared_ptr<texture_object> make_pattern_frame(uint32_t width, uint32_t height, uint64_t timestamp_ms,
						   uint32_t phase)
{
	const size_t stride = static_cast<size_t>(width) * 3;
	std::vector<uint8_t> pixels(stride * height);
	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			const uint32_t pattern = (x * 31u + y * 17u + ((x ^ y) * 13u) + phase * 47u) & 0xffu;
			const size_t offset = static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3;
			pixels[offset] = static_cast<uint8_t>(pattern);
			pixels[offset + 1] = static_cast<uint8_t>(pattern ^ (x & 0xffu));
			pixels[offset + 2] = static_cast<uint8_t>(pattern ^ (y & 0xffu));
		}
	}

	obs_source_frame source_frame{};
	source_frame.data[0] = pixels.data();
	source_frame.linesize[0] = static_cast<uint32_t>(stride);
	source_frame.width = width;
	source_frame.height = height;
	source_frame.format = VIDEO_FORMAT_BGR3;
	auto texture = std::make_shared<texture_object>();
	texture->scale = 1.0f;
	texture->captured_at_ms = timestamp_ms;
	texture->set_texture_obsframe(&source_frame, 1);
	return texture;
}

bool wait_for_worker_to_hold_tracker_lock(face_tracker_dlib &tracker)
{
	const auto deadline = steady_clock::now() + std::chrono::seconds(10);
	while (steady_clock::now() < deadline) {
		if (tracker.trylock() != 0)
			return true;
		tracker.unlock();
		std::this_thread::sleep_for(std::chrono::microseconds(50));
	}
	return false;
}

bool destroy_while_tracking()
{
	constexpr uint32_t width = 1'920;
	constexpr uint32_t height = 1'080;
	auto *tracker = new face_tracker_dlib();
	auto first = make_pattern_frame(width, height, 100, 0);
	tracker->lock();
	tracker->set_texture(first);
	tracker->set_position(rect_s{500, 300, 1'100, 900, 1.0f});
	tracker->set_landmark_detection(nullptr);
	tracker->unlock();
	if (!tracker->start()) {
		delete tracker;
		return false;
	}
	if (!wait_for_worker_to_hold_tracker_lock(*tracker)) {
		delete tracker;
		return false;
	}
	// Acquiring the mutex after observing the busy worker proves the first frame
	// has left track_main() before the second update is queued.
	tracker->lock();
	first.reset();

	auto second = make_pattern_frame(width, height, 200, 1);
	tracker->set_texture(second);
	tracker->signal();
	tracker->unlock();
	if (wait_for_worker_to_hold_tracker_lock(*tracker)) {
		delete tracker;
		return true;
	}
	delete tracker;
	return false;
}

} // namespace

int main()
{
	if (destroy_while_tracking())
		return 0;
	std::fprintf(stderr, "dlib tracker lifetime smoke failed to exercise worker teardown\n");
	return 1;
}
