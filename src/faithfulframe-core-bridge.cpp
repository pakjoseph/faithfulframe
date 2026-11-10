#include "faithfulframe-core-bridge.hpp"

void faithfulframe_core_bridge::deleter::operator()(ff_session *session) const { ff_session_destroy(session); }

faithfulframe_core_bridge::faithfulframe_core_bridge(const ff_session_config &config, const ff_identity &identity, const ff_generations &generations, ff_result &result)
	: bound_identity(identity)
{
	if (faithfulframe_ffi_abi_version() != FAITHFULFRAME_FFI_ABI_VERSION) {
		result = FF_INVALID_ARGUMENT;
		return;
	}
	ff_session *raw = nullptr;
	result = ff_session_create(&config, &bound_identity, &generations, &raw);
	if (result != FF_OK || !raw)
		return;
	session.reset(raw);
}

faithfulframe_core_bridge::~faithfulframe_core_bridge() = default;

ff_result faithfulframe_core_bridge::snapshot(ff_snapshot &output) const
{
	if (!session)
		return FF_INVALID_ARGUMENT;
	return ff_session_snapshot(session.get(), &output);
}

ff_result faithfulframe_core_bridge::tick(uint64_t monotonic_ms, ff_snapshot &output)
{
	if (!session)
		return FF_INVALID_ARGUMENT;
	return ff_session_tick(session.get(), monotonic_ms, &output);
}

ff_result faithfulframe_core_bridge::publish_candidate(uint64_t monotonic_ms, const ff_candidate &candidate, ff_snapshot &output)
{
	if (!session)
		return FF_INVALID_ARGUMENT;
	return ff_session_publish_candidate(session.get(), monotonic_ms, &candidate, &output);
}

ff_result faithfulframe_core_bridge::apply_command(uint64_t monotonic_ms, const ff_command &command, ff_snapshot &output)
{
	if (!session)
		return FF_INVALID_ARGUMENT;
	return ff_session_apply_command(session.get(), monotonic_ms, &command, &output);
}

ff_result faithfulframe_core_bridge::lifecycle(uint64_t monotonic_ms, const ff_lifecycle &event, ff_snapshot &output)
{
	if (!session)
		return FF_INVALID_ARGUMENT;
	return ff_session_lifecycle(session.get(), monotonic_ms, &event, &output);
}
