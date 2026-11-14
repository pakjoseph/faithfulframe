#include "faithfulframe-candidate-selector.hpp"

#include <cstdio>
#include <cstdlib>

static void check_impl(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "candidate selector check failed at line %d\n", line);
		std::abort();
	}
}

#define check(condition) check_impl((condition), __LINE__)

int main()
{
	// Model refreshes are deferred while the popup is open, preserving the
	// volunteer's keyboard-highlighted candidate row.
	int highlighted_row = 1;
	bool refresh_ran = false;
	check(!faithfulframe::ui::refresh_candidate_selector_model(true, [&] {
		refresh_ran = true;
		highlighted_row = 0;
	}));
	check(!refresh_ran && highlighted_row == 1);
	check(faithfulframe::ui::refresh_candidate_selector_model(false, [&] {
		refresh_ran = true;
		highlighted_row = 0;
	}));
	check(refresh_ran && highlighted_row == 0);
	return 0;
}
