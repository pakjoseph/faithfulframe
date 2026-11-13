#include "faithfulframe-source-identity.hpp"

int main()
{
	if (!faithfulframe::source_identity_matches("source-a", "source-a"))
		return 1;

	// A same-name replacement has a different UUID and must not inherit the
	// previous dock selection. Names are deliberately not part of this API.
	if (faithfulframe::source_identity_matches("source-a", "source-b"))
		return 2;
	if (faithfulframe::source_identity_matches("", "source-b"))
		return 3;
	if (faithfulframe::source_identity_matches("source-a", nullptr))
		return 4;

	return 0;
}
