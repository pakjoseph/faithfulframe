#include "faithfulframe-preview-geometry.hpp"

#include <cmath>
#include <cstdlib>
#include <cstdio>

static void check_impl(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "preview geometry check failed at line %d\n", line);
		std::abort();
	}
}

#define check(condition) check_impl((condition), __LINE__)

static bool near(double actual, double expected)
{
	return std::abs(actual - expected) < 1e-6;
}

int main()
{
	double x = -1.0, y = -1.0;
	check(ff_preview_widget_point(128.0, 72.0, 256.0, 144.0, 1920, 1080, x, y));
	check(near(x, 0.5) && near(y, 0.5));
	check(ff_preview_widget_point(256.0, 144.0, 512.0, 288.0, 1920, 1080, x, y));
	check(near(x, 0.5) && near(y, 0.5)); // HiDPI-scaled dimensions preserve normalized hit position.
	check(!ff_preview_widget_point(4.0, 72.0, 400.0, 144.0, 1920, 1080, x, y));  // horizontal letterbox
	check(!ff_preview_widget_point(128.0, 4.0, 144.0, 256.0, 1920, 1080, x, y)); // vertical letterbox
	double zoom = 0.0;
	check(ff_preview_zoom_factor(ff_rect{0.0f, 0.0f, 1.0f, 1.0f}, 3840, 2160, 1280, 960, zoom));
	check(near(zoom, 1.0)); // Full-source framing remains 1x with aspect-fit padding.
	check(ff_preview_zoom_factor(ff_rect{0.3125f, 0.359375f, 0.375f, 0.28125f}, 3840, 2160, 1280, 960, zoom));
	check(near(zoom, 2.0)); // A 4:3 output crop from 16:9 input uses the crop's fit width.
	check(!ff_preview_zoom_factor(ff_rect{NAN, 0.0f, 0.5f, 0.5f}, 3840, 2160, 1920, 1080, zoom));
	check(!ff_preview_zoom_factor(ff_rect{0.0f, 0.0f, 1.0f, 1.0f}, 0, 2160, 1920, 1080, zoom));

	ff_rect source_crop{0.25f, 0.25f, 0.5f, 0.5f};
	ff_rect source_viewport{0.1f, 0.1f, 0.8f, 0.8f};
	ff_preview_rect_d mapped{};
	uint32_t source_preview_width = 1920, source_preview_height = 1080;
	check(ff_preview_set_full_source_transform(source_crop, source_viewport, source_preview_width,
						   source_preview_height, 3840, 2160));
	check(source_preview_width == 3840 && source_preview_height == 2160);
	check(ff_preview_valid_rect(source_crop) && source_crop.x == 0.0f && source_crop.y == 0.0f &&
	      source_crop.width == 1.0f && source_crop.height == 1.0f);
	check(ff_preview_valid_rect(source_viewport) && source_viewport.x == 0.0f && source_viewport.y == 0.0f &&
	      source_viewport.width == 1.0f && source_viewport.height == 1.0f);
	const ff_rect source_candidate{0.25f, 0.25f, 0.25f, 0.25f};
	check(ff_preview_candidate_rect(source_crop, source_viewport, source_candidate, mapped));
	check(near(mapped.x, 0.25) && near(mapped.y, 0.25) && near(mapped.width, 0.25) && near(mapped.height, 0.25));
	check(!ff_preview_set_full_source_transform(source_crop, source_viewport, source_preview_width,
						    source_preview_height, 0, 2160));

	const ff_rect crop{0.25f, 0.25f, 0.5f, 0.5f};
	const ff_rect viewport{0.0f, 0.125f, 1.0f, 0.75f};
	const ff_rect candidate{0.5f, 0.375f, 0.25f, 0.25f};
	check(ff_preview_candidate_rect(crop, viewport, candidate, mapped));
	check(near(mapped.x, 0.5) && near(mapped.y, 0.3125));
	check(near(mapped.width, 0.5) && near(mapped.height, 0.375));

	const ff_rect edge_candidate{0.0f, 0.25f, 0.375f, 0.25f};
	check(ff_preview_candidate_rect(crop, viewport, edge_candidate, mapped));
	check(near(mapped.x, 0.0) && near(mapped.width, 0.25)); // clipped at crop edge

	const ff_rect outside_candidate{0.0f, 0.0f, 0.1f, 0.1f};
	check(!ff_preview_candidate_rect(crop, viewport, outside_candidate, mapped));
	const ff_rect invalid_candidate{0.95f, 0.5f, 0.1f, 0.2f};
	check(!ff_preview_candidate_rect(crop, viewport, invalid_candidate, mapped));
	const ff_rect non_finite{NAN, 0.2f, 0.1f, 0.1f};
	check(!ff_preview_candidate_rect(crop, viewport, non_finite, mapped));
	check(!ff_preview_widget_point(INFINITY, 1.0, 100.0, 100.0, 1920, 1080, x, y));
	return 0;
}
