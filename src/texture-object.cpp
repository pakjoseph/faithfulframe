#include <obs-module.h>
#include <dlib/array2d/array2d_kernel.h>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include "plugin-macros.generated.h"
#include "texture-object.h"
#include "faithfulframe-worker-log.hpp"

static std::atomic<uint32_t> formats_found{0};

static bool first_format_observation(int format)
{
	if (format < 0 || format >= 32)
		return false;
	const uint32_t mask = uint32_t{1} << static_cast<uint32_t>(format);
	return (formats_found.fetch_or(mask, std::memory_order_relaxed) & mask) == 0;
}

struct texture_object_private_s
{
	std::vector<uint8_t> pixels;
	enum video_format format = VIDEO_FORMAT_NONE;
	uint32_t width = 0;
	uint32_t height = 0;
	size_t stride = 0;
	int scale = 0;
};

texture_object::texture_object()
{
	data = new texture_object_private_s;
}

texture_object::~texture_object()
{
	delete data;
}

static int bytes_per_pixel(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_RGBA:
		return 4;
	case VIDEO_FORMAT_BGR3:
		return 3;
	default:
		return 0;
	}
}

void texture_object::set_texture_obsframe(const struct obs_source_frame *frame, int scale)
{
	auto clear_frame = [this] {
		data->pixels.clear();
		data->format = VIDEO_FORMAT_NONE;
		data->width = 0;
		data->height = 0;
		data->stride = 0;
		data->scale = 0;
	};
	const int pixel_size = frame ? bytes_per_pixel(frame->format) : 0;
	if (!frame || scale <= 0 || frame->width == 0 || frame->height == 0 ||
	    frame->width > static_cast<uint32_t>(INT_MAX) || frame->height > static_cast<uint32_t>(INT_MAX) ||
	    frame->width < static_cast<uint32_t>(scale) || frame->height < static_cast<uint32_t>(scale) ||
	    !frame->data[0] || frame->linesize[0] == 0 || pixel_size == 0) {
		if (frame && pixel_size == 0 && first_format_observation(static_cast<int>(frame->format)))
			faithfulframe_worker_log(LOG_ERROR, "Frame format %d has to be RGB", (int)frame->format);
		clear_frame();
		return;
	}

	if (frame->width > std::numeric_limits<size_t>::max() / static_cast<size_t>(pixel_size)) {
		clear_frame();
		return;
	}
	const size_t row_bytes = static_cast<size_t>(frame->width) * static_cast<size_t>(pixel_size);
	const size_t source_stride = frame->linesize[0];
	const size_t height = frame->height;
	if (source_stride < row_bytes ||
	    (height > 1 && source_stride > std::numeric_limits<size_t>::max() / (height - 1)) ||
	    height > std::numeric_limits<size_t>::max() / row_bytes) {
		clear_frame();
		return;
	}
	const size_t byte_count = row_bytes * height;
	try {
		data->pixels.resize(byte_count);
	} catch (...) {
		clear_frame();
		return;
	}
	for (size_t row = 0; row < height; ++row) {
		std::memcpy(data->pixels.data() + row_bytes * row, frame->data[0] + source_stride * row, row_bytes);
	}
	data->format = frame->format;
	data->width = frame->width;
	data->height = frame->height;
	data->stride = row_bytes;
	data->scale = scale;
}

bool texture_object::get_dlib_rgb_image(dlib::matrix<dlib::rgb_pixel> &img) const
{
	if (data->pixels.empty())
		return false;
	const uint8_t *pixels = data->pixels.data();
	const uint32_t width = data->width;
	const uint32_t height = data->height;
	const size_t stride = data->stride;
	const int scale = data->scale;
	if (scale <= 0 || width > static_cast<uint32_t>(INT_MAX) || height > static_cast<uint32_t>(INT_MAX) ||
	    width < static_cast<uint32_t>(scale) || height < static_cast<uint32_t>(scale) || !pixels || stride == 0 ||
	    height > std::numeric_limits<size_t>::max() / stride ||
	    data->pixels.size() < stride * static_cast<size_t>(height))
		return false;

	const int pixel_size = bytes_per_pixel(data->format);
	if (pixel_size == 0) {
		if (first_format_observation(static_cast<int>(data->format)))
			faithfulframe_worker_log(LOG_ERROR, "Frame format %d has to be RGB", (int)data->format);
		return false;
	}
	if (stride < static_cast<size_t>(width) * static_cast<size_t>(pixel_size))
		return false;

	const bool first_format = first_format_observation(static_cast<int>(data->format));
	if (first_format)
		faithfulframe_worker_log(LOG_INFO, "received frame format=%d", data->format);
	const int image_height = static_cast<int>(height / static_cast<uint32_t>(scale));
	const int image_width = static_cast<int>(width / static_cast<uint32_t>(scale));
	img.set_size(image_height, image_width);
	const size_t row_step = stride * static_cast<size_t>(scale);
	const size_t pixel_step = static_cast<size_t>(pixel_size) * static_cast<size_t>(scale);
	switch (data->format) {
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGR3:
	case VIDEO_FORMAT_RGBA:
		for (int i = 0; i < image_height; i++) {
			const uint8_t *line = pixels + row_step * static_cast<size_t>(i);
			for (int j = 0; j < image_width; j++) {
				const size_t js = static_cast<size_t>(j) * pixel_step;
				if (data->format == VIDEO_FORMAT_RGBA) {
					img(i, j).red = line[js + 0];
					img(i, j).green = line[js + 1];
					img(i, j).blue = line[js + 2];
				} else {
					img(i, j).red = line[js + 2];
					img(i, j).green = line[js + 1];
					img(i, j).blue = line[js + 0];
				}
			}
		}
		break;
	default:
		return false;
	}

	return true;
}
