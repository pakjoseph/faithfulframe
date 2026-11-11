#pragma once

#include <cstdint>
#include <memory>

#include "faithfulframe_ffi.h"

class faithfulframe_core_bridge {
public:
	faithfulframe_core_bridge(const ff_session_config &, const ff_identity &, const ff_generations &,
				  ff_result &result);
	~faithfulframe_core_bridge();
	faithfulframe_core_bridge(const faithfulframe_core_bridge &) = delete;
	faithfulframe_core_bridge &operator=(const faithfulframe_core_bridge &) = delete;
	// All mutating calls must run on the adapter's serialized session executor.
	ff_result snapshot(ff_snapshot &output) const;
	ff_result tick(uint64_t monotonic_ms, ff_snapshot &output);
	ff_result publish_candidate(uint64_t monotonic_ms, const ff_candidate &candidate, ff_snapshot &output);
	ff_result apply_command(uint64_t monotonic_ms, const ff_command &command, ff_snapshot &output);
	ff_result lifecycle(uint64_t monotonic_ms, const ff_lifecycle &event, ff_snapshot &output);
	const ff_identity &binding_identity() const { return bound_identity; }

private:
	struct deleter
	{
		void operator()(ff_session *session) const;
	};
	const ff_identity bound_identity;
	std::unique_ptr<ff_session, deleter> session;
};
