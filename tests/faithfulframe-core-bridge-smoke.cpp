#include "faithfulframe-core-bridge.hpp"

int main()
{
	const ff_session_config config{5000, 500, 8, {0.0f, 0.0f, 1.0f, 1.0f}};
	const ff_identity identity{101, 201};
	ff_generations generations{1, 1, 1, 1, 1};
	ff_result result = FF_OK;
	faithfulframe_core_bridge bridge(config, identity, generations, result);
	if (result != FF_OK)
		return 1;

	ff_snapshot snapshot{};
	if (bridge.tick(1, snapshot) != FF_OK || snapshot.state != FF_STATE_MANUAL)
		return 2;

	ff_command enable{FF_COMMAND_ENABLE_AUTO_FOLLOW, snapshot.identity, snapshot.status_version, generations, 0, 0};
	if (bridge.apply_command(2, enable, snapshot) != FF_OK || snapshot.state != FF_STATE_SEARCHING)
		return 3;

	const ff_candidate candidate{22, 1001, identity, 1, generations, 3, 500, {0.4f, 0.2f, 0.1f, 0.2f}};
	if (bridge.publish_candidate(3, candidate, snapshot) != FF_OK || snapshot.observation_version != 1)
		return 4;

	ff_command select{FF_COMMAND_SELECT_CANDIDATE, snapshot.identity, snapshot.status_version, generations, candidate.handle, candidate.observation_version};
	if (bridge.apply_command(4, select, snapshot) != FF_OK || snapshot.state != FF_STATE_LOCKED || !snapshot.has_selected || snapshot.selected_handle != candidate.handle)
		return 5;

	const auto locked = snapshot;
	if (bridge.apply_command(5, select, snapshot) != FF_OK || snapshot.command_result != FF_COMMAND_REJECTED ||
	    snapshot.rejection_reason != FF_REJECTION_STALE_COMMAND || snapshot.state != locked.state ||
	    snapshot.status_version != locked.status_version || snapshot.crop.x != locked.crop.x ||
	    snapshot.crop.y != locked.crop.y || snapshot.crop.width != locked.crop.width ||
	    snapshot.crop.height != locked.crop.height)
		return 6;

	++generations.stream;
	++generations.geometry;
	++generations.tracker;
	const ff_lifecycle geometry_changed{FF_LIFECYCLE_GEOMETRY_CHANGED, snapshot.identity, generations};
	if (bridge.lifecycle(6, geometry_changed, snapshot) != FF_OK || snapshot.state != FF_STATE_MANUAL ||
	    snapshot.has_selected || snapshot.generations.geometry != generations.geometry)
		return 7;
	const auto after_geometry = snapshot;
	ff_generations wrong_scene_generations = snapshot.generations;
	++wrong_scene_generations.session;
	++wrong_scene_generations.tracker;
	const ff_lifecycle wrong_identity_scene{FF_LIFECYCLE_SCENE_ACTIVE, ff_identity{999, 999}, wrong_scene_generations};
	if (bridge.lifecycle(7, wrong_identity_scene, snapshot) != FF_OK ||
	    snapshot.command_result != FF_COMMAND_REJECTED || snapshot.rejection_reason != FF_REJECTION_STALE_LIFECYCLE ||
	    snapshot.status_version != after_geometry.status_version || snapshot.state != after_geometry.state ||
	    snapshot.crop.x != after_geometry.crop.x || snapshot.crop.y != after_geometry.crop.y ||
	    snapshot.crop.width != after_geometry.crop.width || snapshot.crop.height != after_geometry.crop.height)
		return 8;
	const ff_lifecycle inexact_scene{FF_LIFECYCLE_SCENE_ACTIVE, snapshot.identity, snapshot.generations};
	if (bridge.lifecycle(7, inexact_scene, snapshot) != FF_OK ||
	    snapshot.command_result != FF_COMMAND_REJECTED || snapshot.rejection_reason != FF_REJECTION_INVALID_LIFECYCLE ||
	    snapshot.status_version != after_geometry.status_version || snapshot.state != after_geometry.state)
		return 9;

	generations = snapshot.generations;
	++generations.source;
	++generations.session;
	++generations.stream;
	++generations.geometry;
	++generations.tracker;
	const ff_lifecycle rebind{FF_LIFECYCLE_SOURCE_REBOUND, snapshot.identity, generations};
	if (bridge.lifecycle(7, rebind, snapshot) != FF_OK || !snapshot.closed || snapshot.state != FF_STATE_MANUAL)
		return 10;
	const auto closed = snapshot;
	if (bridge.apply_command(8, enable, snapshot) != FF_OK ||
	    snapshot.command_result != FF_COMMAND_REJECTED || snapshot.rejection_reason != FF_REJECTION_SESSION_CLOSED ||
	    !snapshot.closed || snapshot.status_version != closed.status_version || snapshot.state != closed.state)
		return 11;

	const ff_identity other_identity{102, 202};
	ff_result other_result = FF_OK;
	faithfulframe_core_bridge other(config, other_identity, ff_generations{1, 1, 1, 1, 1}, other_result);
	if (other_result != FF_OK)
		return 12;
	ff_snapshot other_snapshot{};
	if (other.snapshot(other_snapshot) != FF_OK)
		return 13;
	const auto other_before = other_snapshot;
	if (other.publish_candidate(4, candidate, other_snapshot) != FF_OK ||
	    other_snapshot.command_result != FF_COMMAND_REJECTED || other_snapshot.state != other_before.state ||
	    other_snapshot.status_version != other_before.status_version ||
	    other_snapshot.crop.x != other_before.crop.x || other_snapshot.crop.y != other_before.crop.y ||
	    other_snapshot.crop.width != other_before.crop.width || other_snapshot.crop.height != other_before.crop.height)
		return 14;
	if (other.apply_command(2, select, other_snapshot) != FF_OK ||
	    other_snapshot.command_result != FF_COMMAND_REJECTED || other_snapshot.state != other_before.state ||
	    other_snapshot.status_version != other_before.status_version)
		return 15;

	const ff_identity invalid_identity{0, 303};
	ff_result invalid_result = FF_OK;
	faithfulframe_core_bridge invalid(config, invalid_identity, generations, invalid_result);
	if (invalid_result != FF_INVALID_ARGUMENT)
		return 16;
	return 0;
}
