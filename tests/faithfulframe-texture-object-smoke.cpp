#include "texture-object.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

extern "C" void faithfulframe_worker_log(int, const char *, ...) {}

namespace {
bool set_pixel(obs_source_frame &frame, uint32_t x, uint32_t y, uint8_t b, uint8_t g, uint8_t r, uint8_t pixel_size)
{
	if (!frame.data[0] || pixel_size < 3 || x >= frame.width || y >= frame.height)
		return false;
	const size_t offset = static_cast<size_t>(y) * frame.linesize[0] + static_cast<size_t>(x) * pixel_size;
	const bool rgba = frame.format == VIDEO_FORMAT_RGBA;
	frame.data[0][offset + 0] = rgba ? r : b;
	frame.data[0][offset + 1] = g;
	frame.data[0][offset + 2] = rgba ? b : r;
	if (pixel_size == 4)
		frame.data[0][offset + 3] = 255;
	return true;
}

bool bgrx_conversion_and_parallel_reads()
{
	constexpr uint32_t width = 4;
	constexpr uint32_t height = 4;
	constexpr uint32_t stride = width * 4 + 4;
	std::vector<uint8_t> pixels(stride * height);
	obs_source_frame frame{};
	frame.format = VIDEO_FORMAT_BGRX;
	frame.width = width;
	frame.height = height;
	frame.linesize[0] = stride;
	frame.data[0] = pixels.data();
	for (uint32_t y = 0; y < frame.height; ++y) {
		for (uint32_t x = 0; x < frame.width; ++x) {
			if (!set_pixel(frame, x, y, static_cast<uint8_t>(x + y), static_cast<uint8_t>(10 + x),
				       static_cast<uint8_t>(20 + y), 4))
				return false;
		}
	}

	texture_object texture;
	texture.set_texture_obsframe(&frame, 2);
	std::fill(pixels.begin(), pixels.end(), 0);
	frame.data[0] = nullptr;

	auto read_and_check = [&texture] {
		dlib::matrix<dlib::rgb_pixel> image;
		return texture.get_dlib_rgb_image(image) && image.nr() == 2 && image.nc() == 2 &&
		       image(0, 0).red == 20 && image(0, 0).green == 10 && image(0, 0).blue == 0 &&
		       image(1, 1).red == 22 && image(1, 1).green == 12 && image(1, 1).blue == 4;
	};
	if (!read_and_check())
		return false;

	std::array<bool, 4> results{};
	std::vector<std::thread> readers;
	for (size_t i = 0; i < results.size(); ++i)
		readers.emplace_back([&results, &read_and_check, i] { results[i] = read_and_check(); });
	for (auto &reader : readers)
		reader.join();
	for (bool result : results) {
		if (!result)
			return false;
	}
	return true;
}

bool bgr3_and_rgba_conversions()
{
	for (const auto [format, pixel_size] :
	     {std::pair{VIDEO_FORMAT_BGR3, uint8_t{3}}, std::pair{VIDEO_FORMAT_RGBA, uint8_t{4}}}) {
		std::vector<uint8_t> pixels(static_cast<size_t>(2 * 2 * pixel_size));
		obs_source_frame frame{};
		frame.format = format;
		frame.width = 2;
		frame.height = 2;
		frame.linesize[0] = 2 * pixel_size;
		frame.data[0] = pixels.data();
		if (!set_pixel(frame, 0, 0, 30, 20, 10, pixel_size))
			return false;
		texture_object texture;
		texture.set_texture_obsframe(&frame, 1);
		dlib::matrix<dlib::rgb_pixel> image;
		if (!texture.get_dlib_rgb_image(image) || image.nr() != 2 || image.nc() != 2 || image(0, 0).red != 10 ||
		    image(0, 0).green != 20 || image(0, 0).blue != 30)
			return false;
	}
	return true;
}

bool unsupported_and_invalid_frames_fail_closed()
{
	std::vector<uint8_t> pixels(4 * 2 * 2);
	obs_source_frame frame{};
	frame.format = VIDEO_FORMAT_BGRA;
	frame.width = 2;
	frame.height = 2;
	frame.linesize[0] = 2 * 4;
	frame.data[0] = pixels.data();
	if (!set_pixel(frame, 0, 0, 1, 2, 3, 4))
		return false;
	texture_object texture;
	texture.set_texture_obsframe(&frame, 1);
	dlib::matrix<dlib::rgb_pixel> image;
	if (!texture.get_dlib_rgb_image(image))
		return false;
	frame.linesize[0] = 1;
	texture.set_texture_obsframe(&frame, 1);
	if (texture.get_dlib_rgb_image(image))
		return false;

	frame.format = VIDEO_FORMAT_I420;
	frame.linesize[0] = 2;
	texture.set_texture_obsframe(&frame, 1);
	if (texture.get_dlib_rgb_image(image))
		return false;

	frame.format = VIDEO_FORMAT_BGRA;
	frame.linesize[0] = 2 * 4;
	texture.set_texture_obsframe(nullptr, 1);
	return !texture.get_dlib_rgb_image(image);
}
} // namespace

int main()
{
	if (!bgrx_conversion_and_parallel_reads()) {
		std::fprintf(stderr, "BGRX conversion or concurrent reads failed\n");
		return 1;
	}
	if (!bgr3_and_rgba_conversions()) {
		std::fprintf(stderr, "BGR3 or RGBA conversion failed\n");
		return 2;
	}
	if (!unsupported_and_invalid_frames_fail_closed()) {
		std::fprintf(stderr, "Unsupported or invalid frame was not rejected\n");
		return 3;
	}
	return 0;
}
