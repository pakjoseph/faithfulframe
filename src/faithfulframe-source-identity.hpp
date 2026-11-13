#pragma once

#include <cstring>

namespace faithfulframe {

// Names are presentation labels and can be reused after a source is removed.
// An empty or changed UUID must therefore never match a prior selection.
inline bool source_identity_matches(const char *expected_uuid, const char *actual_uuid)
{
	return expected_uuid && *expected_uuid && actual_uuid && *actual_uuid &&
	       std::strcmp(expected_uuid, actual_uuid) == 0;
}

} // namespace faithfulframe
