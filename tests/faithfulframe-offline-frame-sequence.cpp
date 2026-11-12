#include "faithfulframe-offline-frame-sequence.hpp"

#include <charconv>
#include <cctype>
#include <exception>
#include <limits>
#include <sstream>
#include <string_view>

namespace faithfulframe::offline {
namespace {

bool parse_timestamp(std::string_view text, uint64_t &value)
{
	if (text.empty())
		return false;
	const char *begin = text.data();
	const char *end = begin + text.size();
	const auto parsed = std::from_chars(begin, end, value, 10);
	return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_dimension(const std::string &token, uint32_t &value)
{
	uint64_t parsed_value = 0;
	if (!parse_timestamp(token, parsed_value) || parsed_value == 0 || parsed_value > 16384)
		return false;
	value = static_cast<uint32_t>(parsed_value);
	return true;
}

bool read_token(std::istream &input, std::string &token)
{
	token.clear();
	while (true) {
		const int next = input.peek();
		if (next == EOF)
			return false;
		if (std::isspace(static_cast<unsigned char>(next))) {
			input.get();
			continue;
		}
		if (next == '#') {
			std::string ignored;
			std::getline(input, ignored);
			continue;
		}
		break;
	}

	while (true) {
		const int next = input.peek();
		if (next == EOF || next == '#' || std::isspace(static_cast<unsigned char>(next)))
			break;
		token.push_back(static_cast<char>(input.get()));
	}
	if (token.empty())
		return false;

	const int delimiter = input.get();
	if (delimiter == '#') {
		std::string ignored;
		std::getline(input, ignored);
	} else if (delimiter == '\r' && input.peek() == '\n') {
		input.get();
	} else if (delimiter == EOF || !std::isspace(static_cast<unsigned char>(delimiter))) {
		return false;
	}
	return true;
}

bool parse_positive_integer(const std::string &token, uint32_t &value)
{
	uint64_t parsed_value = 0;
	if (!parse_timestamp(token, parsed_value) || parsed_value == 0 ||
	    parsed_value > std::numeric_limits<uint32_t>::max())
		return false;
	value = static_cast<uint32_t>(parsed_value);
	return true;
}

std::filesystem::path path_from_utf8(const std::string &text)
{
	std::u8string encoded;
	encoded.reserve(text.size());
	for (unsigned char byte : text)
		encoded.push_back(static_cast<char8_t>(byte));
	return std::filesystem::path(encoded);
}

} // namespace

bool read_manifest(std::istream &input, const std::filesystem::path &manifest_directory,
		   std::vector<frame_spec> &frames, std::string &error)
{
	std::vector<frame_spec> parsed_frames;
	std::string line;
	size_t line_number = 0;
	uint64_t previous_timestamp = 0;
	bool have_timestamp = false;
	while (std::getline(input, line)) {
		++line_number;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t first = line.find_first_not_of(" \t");
		if (first == std::string::npos || line[first] == '#')
			continue;
		if (line == "monotonic_ms\tframe_path")
			continue;
		const size_t separator = line.find('\t', first);
		if (separator == std::string::npos) {
			error = "manifest line " + std::to_string(line_number) + " must contain a tab separator";
			return false;
		}
		const size_t path_start = line.find_first_not_of(" \t", separator + 1);
		const size_t timestamp_end = line.find_last_not_of(" \t", separator - 1);
		if (path_start == std::string::npos || timestamp_end == std::string::npos) {
			error = "manifest line " + std::to_string(line_number) +
				" has an empty timestamp or frame path";
			return false;
		}
		uint64_t timestamp = 0;
		if (!parse_timestamp(std::string_view(line).substr(first, timestamp_end - first + 1), timestamp)) {
			error = "manifest line " + std::to_string(line_number) + " has an invalid monotonic timestamp";
			return false;
		}
		if (have_timestamp && timestamp <= previous_timestamp) {
			error = "manifest timestamps must be strictly increasing (line " + std::to_string(line_number) +
				")";
			return false;
		}
		std::filesystem::path frame_path;
		try {
			frame_path = path_from_utf8(line.substr(path_start));
			if (frame_path.is_relative())
				frame_path = manifest_directory / frame_path;
		} catch (const std::exception &) {
			error = "manifest line " + std::to_string(line_number) + " has an invalid UTF-8 frame path";
			return false;
		}
		parsed_frames.push_back(frame_spec{timestamp, std::move(frame_path)});
		previous_timestamp = timestamp;
		have_timestamp = true;
	}
	if (input.bad()) {
		error = "failed while reading the frame manifest";
		return false;
	}
	if (parsed_frames.empty()) {
		error = "manifest contains no frame entries";
		return false;
	}
	frames = std::move(parsed_frames);
	error.clear();
	return true;
}

bool read_ppm(std::istream &input, rgb_frame &frame, std::string &error)
{
	std::string magic;
	std::string width_token;
	std::string height_token;
	std::string max_value_token;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t max_value = 0;
	if (!read_token(input, magic) || magic != "P6" || !read_token(input, width_token) ||
	    !parse_dimension(width_token, width) || !read_token(input, height_token) ||
	    !parse_dimension(height_token, height) || !read_token(input, max_value_token) ||
	    !parse_positive_integer(max_value_token, max_value) || max_value != 255) {
		error = "expected a binary P6 PPM with dimensions up to 16384 and max value 255";
		return false;
	}
	const uint64_t byte_count = static_cast<uint64_t>(width) * height * 3;
	if (byte_count > std::numeric_limits<size_t>::max() || byte_count > 256ULL * 1024 * 1024) {
		error = "PPM dimensions exceed the 256 MiB frame limit";
		return false;
	}
	rgb_frame parsed_frame;
	parsed_frame.width = width;
	parsed_frame.height = height;
	try {
		parsed_frame.pixels.resize(static_cast<size_t>(byte_count));
	} catch (const std::exception &) {
		error = "could not allocate memory for PPM pixel data";
		return false;
	}
	if (!input.read(reinterpret_cast<char *>(parsed_frame.pixels.data()),
			static_cast<std::streamsize>(parsed_frame.pixels.size()))) {
		error = "PPM pixel data is truncated";
		return false;
	}
	if (input.peek() != EOF) {
		error = "PPM contains trailing data; provide one frame per file";
		return false;
	}
	frame = std::move(parsed_frame);
	error.clear();
	return true;
}

} // namespace faithfulframe::offline
