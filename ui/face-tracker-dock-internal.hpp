#pragma once
#include <obs-module.h>
#include <obs-frontend-api.h>
#include "util/threading.h"
#include "faithfulframe-preview-render-stamp.hpp"

struct face_tracker_dock_s
{
	obs_display_t *disp;
	pthread_mutex_t mutex;
	volatile long ref;

	obs_source_t *src_monitor;
	faithfulframe_preview_render_stamp preview_render_stamp{};
};

static inline void face_tracker_dock_addref(struct face_tracker_dock_s *data)
{
	if (!data)
		return;
	os_atomic_inc_long(&data->ref);
}

struct face_tracker_dock_s *face_tracker_dock_create();
void face_tracker_dock_destroy(struct face_tracker_dock_s *data);

static inline void face_tracker_dock_release(struct face_tracker_dock_s *data)
{
	if (!data)
		return;
	if (os_atomic_dec_long(&data->ref) == 0)
		face_tracker_dock_destroy(data);
}
