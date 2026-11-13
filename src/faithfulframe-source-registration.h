#ifndef FAITHFULFRAME_SOURCE_REGISTRATION_H
#define FAITHFULFRAME_SOURCE_REGISTRATION_H

#include <stdbool.h>
#include <stddef.h>
#include "faithfulframe-source-ids.h"

typedef bool (*faithfulframe_source_type_is_registered_fn)(const char *id);

static const char *const faithfulframe_source_type_ids[] = {
	FAITHFULFRAME_SOURCE_ID_FILTER,
	FAITHFULFRAME_SOURCE_ID_SOURCE,
	FAITHFULFRAME_SOURCE_ID_PTZ,
	FAITHFULFRAME_SOURCE_ID_MONITOR,
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
