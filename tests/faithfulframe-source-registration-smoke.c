#include "faithfulframe-source-registration.h"

#include <stdio.h>
#include <string.h>

static const char *registered_ids[4];
static size_t registered_count;

static bool is_registered(const char *id)
{
	for (size_t i = 0; i < registered_count; ++i) {
		if (strcmp(registered_ids[i], id) == 0)
			return true;
	}
	return false;
}

static void reset_registry(void)
{
	registered_count = 0;
}

static void register_id(const char *id)
{
	if (registered_count >= sizeof(registered_ids) / sizeof(registered_ids[0])) {
		fprintf(stderr, "test registry capacity exceeded\n");
		return;
	}
	registered_ids[registered_count++] = id;
}

int main(void)
{
	reset_registry();
	if (faithfulframe_first_registered_source_type(is_registered) != NULL ||
	    faithfulframe_first_registered_source_type(NULL) != NULL) {
		fprintf(stderr, "empty registry or null callback unexpectedly reported a conflict\n");
		return 1;
	}

	for (size_t i = 0; i < sizeof(faithfulframe_source_type_ids) / sizeof(faithfulframe_source_type_ids[0]); ++i) {
		reset_registry();
		register_id(faithfulframe_source_type_ids[i]);
		const char *conflict = faithfulframe_first_registered_source_type(is_registered);
		if (!conflict || strcmp(conflict, faithfulframe_source_type_ids[i]) != 0) {
			fprintf(stderr, "registered source type %s was not reported\n",
				faithfulframe_source_type_ids[i]);
			return 2;
		}
	}

	reset_registry();
	register_id(faithfulframe_source_type_ids[3]);
	register_id(faithfulframe_source_type_ids[1]);
	const char *conflict = faithfulframe_first_registered_source_type(is_registered);
	if (!conflict || strcmp(conflict, faithfulframe_source_type_ids[1]) != 0) {
		fprintf(stderr, "multiple conflicts did not return the first source type\n");
		return 3;
	}

	return 0;
}
