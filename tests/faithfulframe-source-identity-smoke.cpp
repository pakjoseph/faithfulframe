#include "faithfulframe-source-identity.hpp"

#include <cstring>

int main()
{
	if (!faithfulframe::source_identity_matches("source-a", "source-a"))
		return 1;

	// A same-name replacement has a different UUID and must not inherit the
	// previous selection. Names are deliberately not part of this API.
	const char *previous_name = "Sanctuary Camera";
	const char *replacement_name = "Sanctuary Camera";
	if (std::strcmp(previous_name, replacement_name) != 0)
		return 2;
	if (faithfulframe::source_identity_matches("source-a", "source-b"))
		return 3;
	if (faithfulframe::source_identity_matches("", "source-b"))
		return 4;
	if (faithfulframe::source_identity_matches("source-a", nullptr))
		return 5;

	return 0;
}
