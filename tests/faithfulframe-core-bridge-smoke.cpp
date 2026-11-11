#include "faithfulframe-core-bridge.hpp"

#include <cmath>
#include <cstddef>

static_assert(sizeof(ff_generations) == 40);
static_assert(sizeof(ff_identity) == 16);
static_assert(sizeof(ff_rect) == 16);
static_assert(sizeof(ff_session_config) == 72);
static_assert(offsetof(ff_session_config, dead_zone_fraction) == 60);
static_assert(offsetof(ff_session_config, zoom_smoothing) == 64);
static_assert(sizeof(ff_candidate) == 112);
static_assert(sizeof(ff_command) == 88);
static_assert(sizeof(ff_lifecycle) == 72);
static_assert(sizeof(ff_snapshot) == 152);
static_assert(offsetof(ff_candidate, generations) == 40);
static_assert(offsetof(ff_candidate, rect) == 96);
static_assert(offsetof(ff_command, generations) == 32);
static_assert(offsetof(ff_lifecycle, generations) == 24);
static_assert(offsetof(ff_snapshot, crop) == 76);
static_assert(offsetof(ff_snapshot, generations) == 112);

int main()
{
	const ff_session_config config{5000, 500,  8,     3840,  2160,  1920,  1080, 0.45f,
				       1.0f, 4.0f, 0.25f, 0.08f, 0.08f, 0.05f, 0.25f};
	const ff_identity identity{101, 201};
	ff_generations generations{1, 1, 1, 1, 1};
	ff_result zero_status_result = FF_OK;
	faithfulframe_core_bridge zero_status_bridge(config, identity, generations, zero_status_result);
	if (zero_status_result != FF_OK)
		return 18;
	ff_snapshot zero_status_snapshot{};
	const ff_command initial_freeze{FF_COMMAND_FREEZE, identity, 0, generations, 0, 0};
	if (zero_status_bridge.apply_command(0, initial_freeze, zero_status_snapshot) != FF_OK ||
	    zero_status_snapshot.state != FF_STATE_FROZEN || zero_status_snapshot.status_version != 1)
		return 19;

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

	ff_command select{
		FF_COMMAND_SELECT_CANDIDATE,  snapshot.identity, snapshot.status_version, generations, candidate.handle,
		candidate.observation_version};
	if (bridge.apply_command(4, select, snapshot) != FF_OK || snapshot.state != FF_STATE_LOCKED ||
	    !snapshot.has_selected || snapshot.selected_handle != candidate.handle)
		return 5;
	if (snapshot.crop.width == candidate.rect.width || snapshot.crop.height == candidate.rect.height ||
	    std::abs(snapshot.crop.width / snapshot.crop.height - 1.0f) > 0.001f || snapshot.viewport.width != 1.0f ||
	    snapshot.viewport.height != 1.0f)
		return 17;

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
	const ff_lifecycle geometry_changed{FF_LIFECYCLE_GEOMETRY_CHANGED, snapshot.identity, generations, 1280, 960};
	if (bridge.lifecycle(6, geometry_changed, snapshot) != FF_OK || snapshot.state != FF_STATE_MANUAL ||
	    snapshot.has_selected || snapshot.generations.geometry != generations.geometry ||
	    snapshot.crop.width != 1.0f || snapshot.crop.height != 1.0f ||
	    std::abs(snapshot.viewport.width - 0.75f) > 0.001f || std::abs(snapshot.viewport.x - 0.125f) > 0.001f)
		return 7;
	const auto after_geometry = snapshot;
	ff_generations wrong_scene_generations = snapshot.generations;
	++wrong_scene_generations.session;
	++wrong_scene_generations.tracker;
	const ff_lifecycle wrong_identity_scene{FF_LIFECYCLE_SCENE_ACTIVE, ff_identity{999, 999},
						wrong_scene_generations};
	if (bridge.lifecycle(7, wrong_identity_scene, snapshot) != FF_OK ||
	    snapshot.command_result != FF_COMMAND_REJECTED ||
	    snapshot.rejection_reason != FF_REJECTION_STALE_LIFECYCLE ||
	    snapshot.status_version != after_geometry.status_version || snapshot.state != after_geometry.state ||
	    snapshot.crop.x != after_geometry.crop.x || snapshot.crop.y != after_geometry.crop.y ||
	    snapshot.crop.width != after_geometry.crop.width || snapshot.crop.height != after_geometry.crop.height)
		return 8;
	const ff_lifecycle inexact_scene{FF_LIFECYCLE_SCENE_ACTIVE, snapshot.identity, snapshot.generations};
	if (bridge.lifecycle(7, inexact_scene, snapshot) != FF_OK || snapshot.command_result != FF_COMMAND_REJECTED ||
	    snapshot.rejection_reason != FF_REJECTION_INVALID_LIFECYCLE ||
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
	if (bridge.apply_command(8, enable, snapshot) != FF_OK || snapshot.command_result != FF_COMMAND_REJECTED ||
	    snapshot.rejection_reason != FF_REJECTION_SESSION_CLOSED || !snapshot.closed ||
	    snapshot.status_version != closed.status_version || snapshot.state != closed.state)
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
	    other_snapshot.crop.width != other_before.crop.width ||
	    other_snapshot.crop.height != other_before.crop.height)
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

	ff_session_config deadline_config = config;
	deadline_config.loss_grace_ms = 1500;
	const ff_identity deadline_identity{104, 204};
	const ff_generations deadline_generations{1, 1, 1, 1, 1};
	ff_result deadline_result = FF_OK;
	faithfulframe_core_bridge deadline_bridge(deadline_config, deadline_identity, deadline_generations,
						  deadline_result);
	ff_snapshot deadline_snapshot{};
	if (deadline_result != FF_OK || deadline_bridge.tick(1, deadline_snapshot) != FF_OK)
		return 20;
	const ff_command deadline_enable{FF_COMMAND_ENABLE_AUTO_FOLLOW,
					 deadline_identity,
					 deadline_snapshot.status_version,
					 deadline_generations,
					 0,
					 0};
	if (deadline_bridge.apply_command(2, deadline_enable, deadline_snapshot) != FF_OK)
		return 21;
	const ff_candidate deadline_candidate{31, 3001, deadline_identity,       1, deadline_generations,
					      3,  500,  {0.4f, 0.2f, 0.1f, 0.2f}};
	if (deadline_bridge.publish_candidate(3, deadline_candidate, deadline_snapshot) != FF_OK)
		return 22;
	const ff_command deadline_select{FF_COMMAND_SELECT_CANDIDATE,      deadline_identity,
					 deadline_snapshot.status_version, deadline_generations,
					 deadline_candidate.handle,        deadline_candidate.observation_version};
	if (deadline_bridge.apply_command(4, deadline_select, deadline_snapshot) != FF_OK ||
	    deadline_snapshot.state != FF_STATE_LOCKED || deadline_bridge.tick(504, deadline_snapshot) != FF_OK ||
	    deadline_snapshot.state != FF_STATE_SEARCHING || !deadline_snapshot.has_loss_deadline ||
	    deadline_snapshot.loss_deadline_ms != 2004)
		return 23;
	if (deadline_bridge.tick(2004, deadline_snapshot) != FF_OK || deadline_snapshot.state != FF_STATE_LOST ||
	    deadline_snapshot.has_loss_deadline)
		return 24;
	return 0;
}
