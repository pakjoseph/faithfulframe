#ifndef FAITHFULFRAME_SOURCE_REGISTRATION_H
#define FAITHFULFRAME_SOURCE_REGISTRATION_H

#include <stdbool.h>
#include <stddef.h>

typedef bool (*faithfulframe_source_type_is_registered_fn)(const char *id);

static const char *const faithfulframe_source_type_ids[] = {
	"face_tracker_filter",
	"face_tracker_source",
	"face_tracker_ptz",
	"face_tracker_monitor",
};

static inline const char *
faithfulframe_first_registered_source_type(faithfulframe_source_type_is_registered_fn is_registered)
{
	if (!is_registered)
		return NULL;

	for (size_t i = 0; i < sizeof(faithfulframe_source_type_ids) / sizeof(faithfulframe_source_type_ids[0]); ++i) {
		if (is_registered(faithfulframe_source_type_ids[i]))
			return faithfulframe_source_type_ids[i];
	}

	return NULL;
}

#endif
