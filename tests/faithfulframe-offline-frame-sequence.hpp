#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <vector>

namespace faithfulframe::offline {

struct frame_spec
{
	uint64_t monotonic_ms = 0;
	std::filesystem::path path;
};

struct rgb_frame
{
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> pixels;
};

bool read_manifest(std::istream &input, const std::filesystem::path &manifest_directory,
		   std::vector<frame_spec> &frames, std::string &error);
bool read_ppm(std::istream &input, rgb_frame &frame, std::string &error);

} // namespace faithfulframe::offline
