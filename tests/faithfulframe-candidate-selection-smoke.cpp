#include "faithfulframe-candidate-selection.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

static void check_impl(bool condition, int line)
{
	if (!condition) {
		std::fprintf(stderr, "candidate selection check failed at line %d\n", line);
		std::abort();
	}
}

#define check(condition) check_impl((condition), __LINE__)

int main()
{
	const ff_identity identity{101, 201};
	const ff_generations generations{1, 2, 3, 4, 5};
	const std::array<uint64_t, 3> handles{11, 22, 33};
	size_t index = 99;
	check(faithfulframe_candidate_restore_index(22, identity, generations, identity, generations, handles.data(),
						    handles.size(), index));
	check(index == 1);

	index = 99;
	check(!faithfulframe_candidate_restore_index(44, identity, generations, identity, generations, handles.data(),
						     handles.size(), index));
	check(index == 99); // A disappeared candidate leaves the UI on its placeholder.
	check(!faithfulframe_candidate_restore_index(0, identity, generations, identity, generations, handles.data(),
						     handles.size(), index));
	check(!faithfulframe_candidate_restore_index(22, identity, generations, identity, generations, nullptr, 0,
						     index));

	const ff_identity other_source{102, 201};
	const ff_identity other_session{101, 202};
	check(!faithfulframe_candidate_restore_index(22, identity, generations, other_source, generations,
						     handles.data(), handles.size(), index));
	check(!faithfulframe_candidate_restore_index(22, identity, generations, other_session, generations,
						     handles.data(), handles.size(), index));

	for (size_t changed_generation = 0; changed_generation < 5; ++changed_generation) {
		ff_generations changed = generations;
		switch (changed_generation) {
		case 0:
			++changed.source;
			break;
		case 1:
			++changed.session;
			break;
		case 2:
			++changed.stream;
			break;
		case 3:
			++changed.geometry;
			break;
		case 4:
			++changed.tracker;
			break;
		}
		check(!faithfulframe_candidate_restore_index(22, identity, generations, identity, changed,
							     handles.data(), handles.size(), index));
	}

	return 0;
}
