#include "faithfulframe-offline-frame-sequence.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace {

bool manifest_order_and_paths()
{
	std::istringstream input(
		"# timestamp and relative frame\nmonotonic_ms\tframe_path\n10\tframe one.ppm\n20\tnested/frame two.ppm\n");
	std::vector<faithfulframe::offline::frame_spec> frames;
	std::string error;
	if (!faithfulframe::offline::read_manifest(input, "/fixture", frames, error) || frames.size() != 2 ||
	    frames[0].monotonic_ms != 10 || frames[0].path != "/fixture/frame one.ppm" ||
	    frames[1].monotonic_ms != 20 || frames[1].path != "/fixture/nested/frame two.ppm")
		return false;
	return true;
}

bool invalid_manifest_is_rejected_without_replacing_output()
{
	std::vector<faithfulframe::offline::frame_spec> frames{{7, "preserved.ppm"}};
	std::istringstream duplicate_timestamps("10\tfirst.ppm\n10\tsecond.ppm\n");
	std::string error;
	if (faithfulframe::offline::read_manifest(duplicate_timestamps, ".", frames, error) || error.empty() ||
	    frames.size() != 1 || frames[0].path != "preserved.ppm")
		return false;
	std::istringstream missing_separator("10 first.ppm\n");
	return !faithfulframe::offline::read_manifest(missing_separator, ".", frames, error) && !error.empty();
}

bool ppm_reads_binary_pixels_and_comments()
{
	std::string contents = "P6\n# color frame\n2 1\n255\n";
	contents.append("\x0a\x20\x23\x04\x05\x06", 6);
	std::istringstream input(contents, std::ios::binary);
	faithfulframe::offline::rgb_frame frame;
	std::string error;
	if (!faithfulframe::offline::read_ppm(input, frame, error) || frame.width != 2 || frame.height != 1 ||
	    frame.pixels != std::vector<uint8_t>({10, 32, 35, 4, 5, 6}))
		return false;
	return true;
}

bool malformed_ppms_fail_closed()
{
	std::string error;
	faithfulframe::offline::rgb_frame frame{1, 1, {9, 8, 7}};
	std::istringstream truncated("P6\n1 1\n255\n\x01\x02", std::ios::binary);
	if (faithfulframe::offline::read_ppm(truncated, frame, error) || error.empty() ||
	    frame.pixels != std::vector<uint8_t>({9, 8, 7}))
		return false;
	std::istringstream wrong_max("P6\n1 1\n65535\n\x01\x02\x03", std::ios::binary);
	if (faithfulframe::offline::read_ppm(wrong_max, frame, error) || error.empty())
		return false;
	std::istringstream unreasonable_dimensions("P6\n16385 1\n255\n", std::ios::binary);
	return !faithfulframe::offline::read_ppm(unreasonable_dimensions, frame, error) && !error.empty();
}

} // namespace

int main()
{
	return manifest_order_and_paths() && invalid_manifest_is_rejected_without_replacing_output() &&
			       ppm_reads_binary_pixels_and_comments() && malformed_ppms_fail_closed()
		       ? 0
		       : 1;
}
