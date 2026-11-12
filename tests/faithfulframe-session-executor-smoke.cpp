#include "faithfulframe-core-bridge.hpp"
#include "faithfulframe-config-guard.hpp"

#include <barrier>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>

namespace {
template<typename Predicate>
bool await_snapshot(const faithfulframe_session_executor &executor, Predicate predicate, ff_snapshot &snapshot)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	do {
		if (executor.try_snapshot(snapshot) && predicate(snapshot))
			return true;
		std::this_thread::yield();
	} while (std::chrono::steady_clock::now() < deadline);
	return false;
}

bool await_worker_unavailable(const faithfulframe_session_executor &executor)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	do {
		if (!executor.worker_available())
			return true;
		std::this_thread::yield();
	} while (std::chrono::steady_clock::now() < deadline);
	return false;
}

bool configuration_revision_guard_is_race_safe()
{
	std::mutex configuration_mutex;
	std::atomic<uint64_t> version{0};
	std::atomic<bool> pending{false};
	uint64_t first_revision = 0;
	{
		std::lock_guard<std::mutex> lock(configuration_mutex);
		first_revision = faithfulframe::advance_configuration_revision_locked(version, pending);
	}
	if (first_revision != 1 || !pending.load(std::memory_order_acquire))
		return false;

	uint64_t second_revision = 0;
	{
		std::lock_guard<std::mutex> lock(configuration_mutex);
		second_revision = faithfulframe::advance_configuration_revision_locked(version, pending);
	}
	if (second_revision != 2 ||
	    faithfulframe::clear_reconfiguration_if_current(configuration_mutex, version, pending, first_revision) ||
	    !pending.load(std::memory_order_acquire))
		return false;
	if (!faithfulframe::clear_reconfiguration_if_current(configuration_mutex, version, pending, second_revision) ||
	    pending.load(std::memory_order_acquire))
		return false;

	uint64_t raced_revision = 0;
	{
		std::lock_guard<std::mutex> lock(configuration_mutex);
		raced_revision = faithfulframe::advance_configuration_revision_locked(version, pending);
	}
	std::barrier start(3);
	uint64_t published_revision = 0;
	std::thread clearer([&] {
		start.arrive_and_wait();
		faithfulframe::clear_reconfiguration_if_current(configuration_mutex, version, pending, raced_revision);
	});
	std::thread publisher([&] {
		start.arrive_and_wait();
		std::lock_guard<std::mutex> lock(configuration_mutex);
		published_revision = faithfulframe::advance_configuration_revision_locked(version, pending);
	});
	start.arrive_and_wait();
	clearer.join();
	publisher.join();
	if (raced_revision != 3 || published_revision != 4 || version.load(std::memory_order_acquire) != 4 ||
	    !pending.load(std::memory_order_acquire))
		return false;

	version.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
	pending.store(false, std::memory_order_release);
	uint64_t exhausted_revision = 0;
	{
		std::lock_guard<std::mutex> lock(configuration_mutex);
		exhausted_revision = faithfulframe::advance_configuration_revision_locked(version, pending);
	}
	return exhausted_revision == std::numeric_limits<uint64_t>::max() &&
	       !faithfulframe::clear_reconfiguration_if_current(configuration_mutex, version, pending,
								std::numeric_limits<uint64_t>::max()) &&
	       pending.load(std::memory_order_acquire);
}
} // namespace

int run_smoke()
{
	faithfulframe_executor_reaper_start();
	if (!configuration_revision_guard_is_race_safe())
		return 23;
	const ff_session_config config{5000, 500,  8,     3840,  2160,  1920,  1080, 0.45f,
				       1.0f, 4.0f, 0.25f, 0.08f, 0.08f, 0.05f, 0.25f};
	const ff_identity identity{101, 201};
	const ff_generations generations{1, 1, 1, 1, 1};
	faithfulframe_session_executor executor(config, identity, generations);
	ff_snapshot snapshot{};
	if (!await_snapshot(executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; }, snapshot))
		return 1;

	const ff_command enable{
		FF_COMMAND_ENABLE_AUTO_FOLLOW, snapshot.identity, snapshot.status_version, snapshot.generations, 0, 0};
	if (!executor.enqueue_command(2, enable) || !await_snapshot(
							    executor,
							    [](const ff_snapshot &value) {
								    return value.command_result == FF_COMMAND_APPLIED &&
									   value.state == FF_STATE_SEARCHING;
							    },
							    snapshot))
		return 2;

	const ff_candidate candidate{22, 1001, identity, 1, generations, 3, 500, {0.4f, 0.2f, 0.1f, 0.2f}};
	if (!executor.enqueue_candidates(3, &candidate, 1) || !await_snapshot(
								      executor,
								      [](const ff_snapshot &value) {
									      return value.observation_version == 1 &&
										     value.state == FF_STATE_SEARCHING;
								      },
								      snapshot))
		return 3;

	const ff_command select{FF_COMMAND_SELECT_CANDIDATE, snapshot.identity, snapshot.status_version,
				snapshot.generations,        candidate.handle,  candidate.observation_version};
	if (!executor.enqueue_command(4, select) || !await_snapshot(
							    executor,
							    [](const ff_snapshot &value) {
								    return value.command_result == FF_COMMAND_APPLIED &&
									   value.state == FF_STATE_LOCKED;
							    },
							    snapshot))
		return 4;
	const ff_snapshot locked = snapshot;

	if (!executor.enqueue_command(5, select) ||
	    !await_snapshot(
		    executor,
		    [](const ff_snapshot &value) {
			    return value.command_result == FF_COMMAND_REJECTED &&
				   value.rejection_reason == FF_REJECTION_STALE_COMMAND;
		    },
		    snapshot))
		return 5;
	if (snapshot.state != locked.state || snapshot.status_version != locked.status_version ||
	    snapshot.selected_handle != locked.selected_handle || snapshot.crop.x != locked.crop.x ||
	    snapshot.crop.y != locked.crop.y || snapshot.crop.width != locked.crop.width ||
	    snapshot.crop.height != locked.crop.height)
		return 6;
	const uint64_t rejected_status_version = snapshot.status_version;
	if (!executor.enqueue_tick(6) ||
	    !await_snapshot(
		    executor,
		    [rejected_status_version](const ff_snapshot &value) {
			    return value.status_version > rejected_status_version &&
				   value.command_result == FF_COMMAND_REJECTED &&
				   value.rejection_reason == FF_REJECTION_STALE_COMMAND;
		    },
		    snapshot) ||
	    snapshot.state != locked.state || snapshot.selected_handle != locked.selected_handle ||
	    snapshot.crop.x != locked.crop.x || snapshot.crop.y != locked.crop.y ||
	    snapshot.crop.width != locked.crop.width || snapshot.crop.height != locked.crop.height)
		return 7;

	executor.shutdown();
	if (!executor.try_snapshot(snapshot) || !snapshot.closed || snapshot.state != FF_STATE_MANUAL ||
	    snapshot.has_selected)
		return 8;
	const ff_executor_diagnostics completed_diagnostics = executor.diagnostics();
	if (completed_diagnostics.queue_depth != 0 || completed_diagnostics.queue_high_water != 1 ||
	    completed_diagnostics.accepted_work != 5 || completed_diagnostics.rejected_work != 0 ||
	    completed_diagnostics.coalesced_work != 0 || completed_diagnostics.last_queue_wait_us > 10'000'000)
		return 15;

	ff_session_config invalid_config = config;
	invalid_config.output_width = 0;
	const ff_identity failed_identity{102, 202};
	faithfulframe_session_executor failed_worker(invalid_config, failed_identity, generations);
	const ff_command command_after_worker_failure{
		FF_COMMAND_ENABLE_AUTO_FOLLOW, failed_identity, 0, generations, 0, 0};
	if (!await_worker_unavailable(failed_worker) || failed_worker.worker_available() ||
	    failed_worker.enqueue_tick(1) || failed_worker.enqueue_command(1, command_after_worker_failure) ||
	    failed_worker.diagnostics().queue_depth != 0)
		return 16;
	failed_worker.shutdown();

	// Cross-thread ingress can acquire the queue mutex in a different order
	// from timestamp sampling. Event time and snapshot publication must remain
	// monotonic in the actual serialized work order.
	faithfulframe_session_executor ordered(config, ff_identity{103, 203}, generations);
	ff_snapshot ordered_snapshot{};
	if (!await_snapshot(
		    ordered, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; }, ordered_snapshot))
		return 20;
	const uint64_t initial_version = ordered_snapshot.status_version;
	if (!ordered.enqueue_tick(100) ||
	    !await_snapshot(
		    ordered,
		    [initial_version](const ff_snapshot &value) { return value.status_version > initial_version; },
		    ordered_snapshot))
		return 21;
	uint64_t first_published_at = 0;
	if (!ordered.try_snapshot(ordered_snapshot, &first_published_at) || first_published_at != 100)
		return 22;
	const uint64_t first_version = ordered_snapshot.status_version;
	if (!ordered.enqueue_tick(99) ||
	    !await_snapshot(
		    ordered, [first_version](const ff_snapshot &value) { return value.status_version > first_version; },
		    ordered_snapshot))
		return 23;
	uint64_t second_published_at = 0;
	if (!ordered.try_snapshot(ordered_snapshot, &second_published_at) || second_published_at != first_published_at)
		return 24;
	ordered.shutdown();

	// Equal monotonic timestamps follow the queue's mutex-linearized FIFO
	// insertion order. Freeze then Resume returns to Manual; reversing the
	// order would leave the session Frozen.
	faithfulframe_session_executor equal_time(config, ff_identity{104, 204}, generations);
	ff_snapshot equal_snapshot{};
	if (!await_snapshot(
		    equal_time, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    equal_snapshot))
		return 33;
	const ff_command equal_freeze{FF_COMMAND_FREEZE,
				      equal_snapshot.identity,
				      equal_snapshot.status_version,
				      equal_snapshot.generations,
				      0,
				      0};
	const ff_command equal_resume{FF_COMMAND_RESUME,
				      equal_snapshot.identity,
				      equal_snapshot.status_version,
				      equal_snapshot.generations,
				      0,
				      0};
	const bool equal_freeze_queued = equal_time.enqueue_command(50, equal_freeze);
	const bool equal_resume_queued = equal_time.enqueue_command(50, equal_resume);
	const bool equal_result = equal_freeze_queued && equal_resume_queued &&
				  await_snapshot(
					  equal_time,
					  [](const ff_snapshot &value) {
						  return value.status_version == 2 && value.state == FF_STATE_MANUAL &&
							 value.command_result == FF_COMMAND_APPLIED;
					  },
					  equal_snapshot);
	if (!equal_result) {
		std::fprintf(stderr, "equal-time commands queued=%d/%d state=%u result=%u rejection=%u version=%llu\n",
			     equal_freeze_queued, equal_resume_queued, equal_snapshot.state,
			     equal_snapshot.command_result, equal_snapshot.rejection_reason,
			     static_cast<unsigned long long>(equal_snapshot.status_version));
		return 34;
	}
	equal_time.shutdown();

	// One candidate batch can contain tracker results with different capture
	// timestamps. Arrival-time ordering is shared, but each fresh face remains
	// available for operator selection regardless of array order.
	const ff_identity multi_identity{105, 205};
	faithfulframe_session_executor multi_face(config, multi_identity, generations);
	ff_snapshot multi_snapshot{};
	if (!await_snapshot(
		    multi_face, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    multi_snapshot))
		return 40;
	const ff_command multi_enable{FF_COMMAND_ENABLE_AUTO_FOLLOW,
				      multi_snapshot.identity,
				      multi_snapshot.status_version,
				      multi_snapshot.generations,
				      0,
				      0};
	const ff_candidate multi_candidates[] = {
		{31, 310, multi_identity, 1, generations, 3, 503, {0.2f, 0.2f, 0.1f, 0.2f}},
		{32, 320, multi_identity, 1, generations, 2, 502, {0.7f, 0.2f, 0.1f, 0.2f}},
	};
	if (!multi_face.enqueue_command(2, multi_enable) ||
	    !await_snapshot(
		    multi_face, [](const ff_snapshot &value) { return value.state == FF_STATE_SEARCHING; },
		    multi_snapshot) ||
	    !multi_face.enqueue_candidates(3, multi_candidates, std::size(multi_candidates)) ||
	    !await_snapshot(
		    multi_face,
		    [](const ff_snapshot &value) {
			    return value.state == FF_STATE_SEARCHING && value.observation_version == 1;
		    },
		    multi_snapshot))
		return 41;
	const ff_command select_second_face{FF_COMMAND_SELECT_CANDIDATE,
					    multi_snapshot.identity,
					    multi_snapshot.status_version,
					    multi_snapshot.generations,
					    32,
					    1};
	if (!multi_face.enqueue_command(4, select_second_face) ||
	    !await_snapshot(
		    multi_face,
		    [](const ff_snapshot &value) {
			    return value.command_result == FF_COMMAND_APPLIED && value.state == FF_STATE_LOCKED &&
				   value.selected_handle == 32;
		    },
		    multi_snapshot))
		return 42;
	multi_face.shutdown();

	// Teardown drops queued detector and operator work, then publishes only the
	// worker-owned destruction transition.
	faithfulframe_session_executor queued(config, ff_identity{102, 202}, generations);
	ff_snapshot queued_snapshot{};
	const ff_command queued_enable{FF_COMMAND_ENABLE_AUTO_FOLLOW, ff_identity{102, 202}, 0, generations, 0, 0};
	if (!queued.enqueue_command(20, queued_enable))
		return 9;
	const ff_candidate queued_candidate{23, 1002, ff_identity{102, 202},   1, generations,
					    21, 500,  {0.3f, 0.2f, 0.2f, 0.3f}};
	if (!queued.enqueue_candidates(21, &queued_candidate, 1))
		return 10;
	const ff_command stale_dock_command{FF_COMMAND_SELECT_CANDIDATE, ff_identity{102, 202}, 0, generations, 23, 1};
	if (!queued.enqueue_command(21, stale_dock_command))
		return 11;
	queued.request_shutdown(FF_LIFECYCLE_SOURCE_DESTROYED);
	if (queued.enqueue_tick(22) || queued.enqueue_command(22, stale_dock_command))
		return 14;
	queued.shutdown();
	if (!queued.try_snapshot(queued_snapshot) || !queued_snapshot.closed || queued_snapshot.has_selected ||
	    queued_snapshot.state != FF_STATE_MANUAL || queued_snapshot.crop_action != FF_CROP_WIDE)
		return 12;
	if (queued_snapshot.generations.source != 2 || queued_snapshot.generations.session != 2 ||
	    queued_snapshot.generations.stream != 2 || queued_snapshot.generations.geometry != 1 ||
	    queued_snapshot.generations.tracker != 2 || queued.enqueue_tick(22) ||
	    queued.enqueue_candidates(22, &queued_candidate, 1))
		return 13;
	const ff_executor_diagnostics teardown_diagnostics = queued.diagnostics();
	if (teardown_diagnostics.queue_depth != 0 || teardown_diagnostics.accepted_work != 3 ||
	    teardown_diagnostics.rejected_work != 4 || teardown_diagnostics.coalesced_work != 0 ||
	    teardown_diagnostics.queue_high_water > 3 || teardown_diagnostics.last_queue_wait_us > 10'000'000)
		return 16;

	// Hold a callback-owned executor reference after the adapter atomically
	// removes its published handle. Teardown begins between command creation
	// and submission; the callback must be able to finish without touching a
	// destroyed executor, and the closed executor must reject that command.
	for (int attempt = 0; attempt < 10; ++attempt) {
		auto owned = std::make_shared<faithfulframe_session_executor>(
			config,
			ff_identity{200u + static_cast<uint64_t>(attempt), 300u + static_cast<uint64_t>(attempt)},
			generations);
		ff_snapshot initial{};
		if (!await_snapshot(
			    *owned, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; }, initial))
			return 17;
		std::weak_ptr<faithfulframe_session_executor> lifetime = owned;
		std::shared_ptr<faithfulframe_session_executor> published = owned;
		owned.reset();

		std::mutex gate_mutex;
		std::condition_variable gate_condition;
		bool command_created = false;
		bool allow_submission = false;
		bool command_accepted = true;
		std::thread delayed_callback([&] {
			auto callback_owner = std::atomic_load_explicit(&published, std::memory_order_acquire);
			ff_command delayed_command{FF_COMMAND_ENABLE_AUTO_FOLLOW,
						   initial.identity,
						   initial.status_version,
						   initial.generations,
						   0,
						   0};
			{
				std::unique_lock<std::mutex> lock(gate_mutex);
				command_created = true;
				gate_condition.notify_one();
				gate_condition.wait(lock, [&] { return allow_submission; });
			}
			command_accepted = callback_owner && callback_owner->enqueue_command(1, delayed_command);
		});

		{
			std::unique_lock<std::mutex> lock(gate_mutex);
			gate_condition.wait(lock, [&] { return command_created; });
		}
		auto retiring = std::atomic_exchange_explicit(
			&published, std::shared_ptr<faithfulframe_session_executor>{}, std::memory_order_acq_rel);
		if (!retiring) {
			{
				std::lock_guard<std::mutex> lock(gate_mutex);
				allow_submission = true;
			}
			gate_condition.notify_one();
			delayed_callback.join();
			return 18;
		}
		retiring->request_shutdown();
		retiring.reset();
		{
			std::lock_guard<std::mutex> lock(gate_mutex);
			allow_submission = true;
		}
		gate_condition.notify_one();
		delayed_callback.join();
		if (command_accepted || !lifetime.expired())
			return 19;
	}

	// A late detector result must not reacquire a target after the stale-lock
	// grace deadline, even when the executor has not received a Tick at that
	// deadline. This exercises timer ordering through the C++/Rust bridge.
	faithfulframe_session_executor deadline_executor(config, ff_identity{401, 501}, generations);
	ff_snapshot deadline_snapshot{};
	if (!await_snapshot(
		    deadline_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    deadline_snapshot))
		return 25;
	const ff_command start_follow{FF_COMMAND_ENABLE_AUTO_FOLLOW,
				      deadline_snapshot.identity,
				      deadline_snapshot.status_version,
				      deadline_snapshot.generations,
				      0,
				      0};
	if (!deadline_executor.enqueue_command(1, start_follow) ||
	    !await_snapshot(
		    deadline_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_SEARCHING; },
		    deadline_snapshot))
		return 26;
	const ff_candidate initial_candidate{77, 900, deadline_snapshot.identity, 1, generations,
					     2,  502, {0.4f, 0.2f, 0.1f, 0.2f}};
	if (!deadline_executor.enqueue_candidates(2, &initial_candidate, 1) ||
	    !await_snapshot(
		    deadline_executor, [](const ff_snapshot &value) { return value.observation_version == 1; },
		    deadline_snapshot))
		return 27;
	const ff_command select_initial{FF_COMMAND_SELECT_CANDIDATE,      deadline_snapshot.identity,
					deadline_snapshot.status_version, deadline_snapshot.generations,
					initial_candidate.handle,         initial_candidate.observation_version};
	if (!deadline_executor.enqueue_command(3, select_initial) ||
	    !await_snapshot(
		    deadline_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_LOCKED; },
		    deadline_snapshot))
		return 28;
	const ff_candidate late_candidate{78,   900,  deadline_snapshot.identity, 2, generations,
					  5503, 6003, {0.5f, 0.2f, 0.1f, 0.2f}};
	if (!deadline_executor.enqueue_candidates(5503, &late_candidate, 1) ||
	    !await_snapshot(
		    deadline_executor,
		    [](const ff_snapshot &value) {
			    return value.observation_version == 2 && value.state == FF_STATE_LOST;
		    },
		    deadline_snapshot) ||
	    deadline_snapshot.has_selected || deadline_snapshot.crop_action != FF_CROP_HOLD)
		return 29;
	deadline_executor.shutdown();

	// Exercise every C++ lifecycle translation against the Rust policy. A
	// generation-table drift would otherwise leave an adapter transition
	// pending after Rust rejects the event.
	struct lifecycle_case
	{
		uint32_t kind;
		ff_generations expected_generations;
		uint32_t expected_state;
		bool expected_closed;
	};
	const ff_generations initial_generations{1, 1, 1, 1, 1};
	const lifecycle_case lifecycle_cases[] = {
		{FF_LIFECYCLE_SCENE_INACTIVE, {1, 2, 1, 1, 2}, FF_STATE_MANUAL, false},
		{FF_LIFECYCLE_SCENE_ACTIVE, {1, 2, 1, 1, 2}, FF_STATE_MANUAL, false},
		{FF_LIFECYCLE_CAMERA_DISCONNECTED, {1, 1, 2, 1, 2}, FF_STATE_MANUAL, false},
		{FF_LIFECYCLE_CAMERA_RESTORED, {1, 1, 2, 1, 2}, FF_STATE_MANUAL, false},
		{FF_LIFECYCLE_SOURCE_REBOUND, {2, 2, 2, 2, 2}, FF_STATE_MANUAL, true},
		{FF_LIFECYCLE_GEOMETRY_CHANGED, {1, 1, 2, 2, 2}, FF_STATE_MANUAL, false},
		{FF_LIFECYCLE_TRACKER_CHANGED, {1, 1, 1, 1, 2}, FF_STATE_LOST, false},
		{FF_LIFECYCLE_FRAMING_CONFIG_CHANGED, {1, 2, 1, 2, 2}, FF_STATE_MANUAL, true},
		{FF_LIFECYCLE_CANDIDATE_POLICY_CHANGED, {1, 2, 1, 1, 2}, FF_STATE_MANUAL, true},
		{FF_LIFECYCLE_ADAPTER_DESTROYED, {1, 2, 1, 1, 2}, FF_STATE_MANUAL, true},
		{FF_LIFECYCLE_SOURCE_DESTROYED, {2, 2, 2, 1, 2}, FF_STATE_MANUAL, true},
		{FF_LIFECYCLE_OBS_SHUTDOWN, {2, 2, 2, 1, 2}, FF_STATE_MANUAL, true},
	};
	auto same_generations = [](const ff_generations &left, const ff_generations &right) {
		return left.source == right.source && left.session == right.session && left.stream == right.stream &&
		       left.geometry == right.geometry && left.tracker == right.tracker;
	};
	for (size_t i = 0; i < std::size(lifecycle_cases); ++i) {
		const auto &test = lifecycle_cases[i];
		const ff_identity lifecycle_identity{600 + i, 700 + i};
		faithfulframe_session_executor lifecycle_executor(config, lifecycle_identity, initial_generations);
		ff_snapshot lifecycle_snapshot{};
		if (!await_snapshot(
			    lifecycle_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
			    lifecycle_snapshot))
			return 30;

		ff_lifecycle lifecycle{};
		lifecycle.kind = test.kind;
		lifecycle.identity = lifecycle_identity;
		if (!faithfulframe_next_lifecycle_generations(test.kind, initial_generations, lifecycle.generations) ||
		    !same_generations(lifecycle.generations, test.expected_generations))
			return 31;
		if (test.kind == FF_LIFECYCLE_GEOMETRY_CHANGED) {
			lifecycle.source_width = 3840;
			lifecycle.source_height = 2160;
		}
		if (!lifecycle_executor.enqueue_lifecycle(10, lifecycle) ||
		    !await_snapshot(
			    lifecycle_executor,
			    [&test, &same_generations](const ff_snapshot &value) {
				    return value.state == test.expected_state && value.closed == test.expected_closed &&
					   same_generations(value.generations, test.expected_generations);
			    },
			    lifecycle_snapshot)) {
			std::fprintf(
				stderr,
				"lifecycle case %zu kind=%u actual state=%u closed=%u generations=%llu/%llu/%llu/%llu/%llu\n",
				i, test.kind, lifecycle_snapshot.state, lifecycle_snapshot.closed,
				static_cast<unsigned long long>(lifecycle_snapshot.generations.source),
				static_cast<unsigned long long>(lifecycle_snapshot.generations.session),
				static_cast<unsigned long long>(lifecycle_snapshot.generations.stream),
				static_cast<unsigned long long>(lifecycle_snapshot.generations.geometry),
				static_cast<unsigned long long>(lifecycle_snapshot.generations.tracker));
			return 32;
		}
		lifecycle_executor.shutdown();
	}

	// Scene deactivation must invalidate a previously locked target. A click
	// captured before deactivation is delivered afterward and must be a strict
	// no-op; activation requires a fresh explicit selection.
	const ff_identity scene_identity{800, 900};
	faithfulframe_session_executor scene_executor(config, scene_identity, initial_generations);
	ff_snapshot scene_snapshot{};
	if (!await_snapshot(
		    scene_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    scene_snapshot))
		return 50;
	const ff_command scene_enable{FF_COMMAND_ENABLE_AUTO_FOLLOW,
				      scene_snapshot.identity,
				      scene_snapshot.status_version,
				      scene_snapshot.generations,
				      0,
				      0};
	const ff_candidate scene_candidate{81, 810, scene_identity,          1, initial_generations,
					   3,  503, {0.4f, 0.2f, 0.1f, 0.2f}};
	if (!scene_executor.enqueue_command(2, scene_enable) ||
	    !await_snapshot(
		    scene_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_SEARCHING; },
		    scene_snapshot) ||
	    !scene_executor.enqueue_candidates(3, &scene_candidate, 1) ||
	    !await_snapshot(
		    scene_executor,
		    [](const ff_snapshot &value) {
			    return value.state == FF_STATE_SEARCHING && value.observation_version == 1;
		    },
		    scene_snapshot))
		return 51;
	const ff_command delayed_scene_selection{FF_COMMAND_SELECT_CANDIDATE,   scene_snapshot.identity,
						 scene_snapshot.status_version, scene_snapshot.generations,
						 scene_candidate.handle,        scene_candidate.observation_version};
	if (!scene_executor.enqueue_command(4, delayed_scene_selection) ||
	    !await_snapshot(
		    scene_executor,
		    [](const ff_snapshot &value) { return value.state == FF_STATE_LOCKED && value.has_selected; },
		    scene_snapshot))
		return 52;
	const ff_snapshot scene_locked = scene_snapshot;
	ff_generations inactive_generations{};
	ff_lifecycle scene_inactive{FF_LIFECYCLE_SCENE_INACTIVE, scene_identity, {}};
	if (!faithfulframe_next_lifecycle_generations(FF_LIFECYCLE_SCENE_INACTIVE, initial_generations,
						      inactive_generations))
		return 53;
	scene_inactive.generations = inactive_generations;
	if (!scene_executor.enqueue_lifecycle(5, scene_inactive) ||
	    !await_snapshot(
		    scene_executor,
		    [&inactive_generations, &same_generations](const ff_snapshot &value) {
			    return value.state == FF_STATE_MANUAL && !value.has_selected &&
				   same_generations(value.generations, inactive_generations);
		    },
		    scene_snapshot) ||
	    scene_snapshot.crop.x != scene_locked.crop.x || scene_snapshot.crop.y != scene_locked.crop.y ||
	    scene_snapshot.crop.width != scene_locked.crop.width ||
	    scene_snapshot.crop.height != scene_locked.crop.height)
		return 54;
	if (!scene_executor.enqueue_command(6, delayed_scene_selection) ||
	    !await_snapshot(
		    scene_executor,
		    [](const ff_snapshot &value) {
			    return value.command_result == FF_COMMAND_REJECTED &&
				   value.rejection_reason == FF_REJECTION_STALE_COMMAND;
		    },
		    scene_snapshot) ||
	    scene_snapshot.state != FF_STATE_MANUAL || scene_snapshot.has_selected ||
	    !same_generations(scene_snapshot.generations, inactive_generations) ||
	    scene_snapshot.crop.x != scene_locked.crop.x || scene_snapshot.crop.y != scene_locked.crop.y ||
	    scene_snapshot.crop.width != scene_locked.crop.width ||
	    scene_snapshot.crop.height != scene_locked.crop.height)
		return 55;
	ff_generations active_generations{};
	ff_lifecycle scene_active{FF_LIFECYCLE_SCENE_ACTIVE, scene_identity, {}};
	if (!faithfulframe_next_lifecycle_generations(FF_LIFECYCLE_SCENE_ACTIVE, inactive_generations,
						      active_generations))
		return 56;
	scene_active.generations = active_generations;
	if (!scene_executor.enqueue_lifecycle(7, scene_active) ||
	    !await_snapshot(
		    scene_executor,
		    [&active_generations, &same_generations](const ff_snapshot &value) {
			    return value.state == FF_STATE_MANUAL && !value.has_selected &&
				   same_generations(value.generations, active_generations);
		    },
		    scene_snapshot))
		return 57;
	const ff_candidate after_scene_candidate{82, 820, scene_identity,          4, active_generations,
						 8,  508, {0.5f, 0.2f, 0.1f, 0.2f}};
	if (!scene_executor.enqueue_candidates(8, &after_scene_candidate, 1) ||
	    !await_snapshot(
		    scene_executor,
		    [](const ff_snapshot &value) {
			    return value.observation_version == 4 && value.state == FF_STATE_MANUAL;
		    },
		    scene_snapshot))
		return 81;
	scene_executor.shutdown();

	auto retired_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{105, 205}, generations);
	ff_snapshot retired_snapshot{};
	if (!await_snapshot(
		    *retired_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    retired_snapshot))
		return 58;
	std::weak_ptr<faithfulframe_session_executor> retired_weak = retired_executor;
	retired_executor->request_shutdown();
	const auto retirement_started = std::chrono::steady_clock::now();
	if (!faithfulframe_retire_session_executor(retired_executor) || retired_executor ||
	    std::chrono::steady_clock::now() - retirement_started > std::chrono::seconds(1))
		return 59;
	const auto retirement_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!retired_weak.expired() && std::chrono::steady_clock::now() < retirement_deadline)
		std::this_thread::yield();
	if (!retired_weak.expired())
		return 60;

	auto unload_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{106, 206}, generations);
	ff_snapshot unload_snapshot{};
	if (!await_snapshot(
		    *unload_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    unload_snapshot))
		return 61;
	std::weak_ptr<faithfulframe_session_executor> unload_weak = unload_executor;
	unload_executor->request_shutdown();
	if (!faithfulframe_retire_session_executor(unload_executor) || unload_executor)
		return 62;

	auto terminal_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{107, 207}, generations);
	ff_snapshot terminal_initial{};
	if (!await_snapshot(
		    *terminal_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    terminal_initial))
		return 64;
	ff_generations destroyed_generations{};
	if (!faithfulframe_next_lifecycle_generations(FF_LIFECYCLE_SOURCE_DESTROYED, terminal_initial.generations,
						      destroyed_generations))
		return 65;
	const ff_lifecycle destroyed_event{FF_LIFECYCLE_SOURCE_DESTROYED, terminal_initial.identity,
					   destroyed_generations};
	if (!terminal_executor->enqueue_lifecycle(1, destroyed_event))
		return 66;
	const ff_command post_destroy_command{FF_COMMAND_ENABLE_AUTO_FOLLOW,
					      terminal_initial.identity,
					      terminal_initial.status_version,
					      terminal_initial.generations,
					      0,
					      0};
	std::weak_ptr<faithfulframe_session_executor> terminal_weak = terminal_executor;
	auto terminal_join = terminal_executor;
	if (!faithfulframe_retire_session_executor(terminal_join) || terminal_join)
		return 67;
	ff_snapshot terminal_snapshot{};
	const auto terminal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < terminal_deadline) {
		if (terminal_executor->try_snapshot(terminal_snapshot) && terminal_snapshot.closed &&
		    !terminal_executor->worker_available())
			break;
		std::this_thread::yield();
	}
	if (!terminal_snapshot.closed || terminal_executor->worker_available() ||
	    !same_generations(terminal_snapshot.generations, destroyed_generations) ||
	    terminal_executor->enqueue_command(2, post_destroy_command))
		return 68;
	terminal_executor.reset();
	struct cleanup_probe
	{
		std::atomic<bool> completed{false};
		std::weak_ptr<faithfulframe_session_executor> prior_executor;
		bool prior_executor_released = false;
		std::thread::id thread_id{};
	} cleanup{false, terminal_weak};
	const std::thread::id destroy_thread = std::this_thread::get_id();
	if (!faithfulframe_retire_cleanup(
		    [](void *context) {
			    auto *probe = static_cast<cleanup_probe *>(context);
			    probe->prior_executor_released = probe->prior_executor.expired();
			    probe->thread_id = std::this_thread::get_id();
			    probe->completed.store(true, std::memory_order_release);
		    },
		    &cleanup))
		return 70;
	// OBS may destroy plugin sources after obs_module_unload returns. The reaper
	// must finish queued work without relying on module unload as a shutdown
	// barrier, and it must continue accepting source teardown afterward.
	const auto unload_drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while ((!cleanup.completed.load(std::memory_order_acquire) || !unload_weak.expired()) &&
	       std::chrono::steady_clock::now() < unload_drain_deadline)
		std::this_thread::yield();
	if (!cleanup.completed.load(std::memory_order_acquire) || !cleanup.prior_executor_released ||
	    cleanup.thread_id == destroy_thread)
		return 71;
	if (!unload_weak.expired())
		return 63;
	auto post_unload_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{111, 211}, generations);
	ff_snapshot post_unload_snapshot{};
	if (!await_snapshot(
		    *post_unload_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    post_unload_snapshot))
		return 82;
	std::weak_ptr<faithfulframe_session_executor> post_unload_weak = post_unload_executor;
	post_unload_executor->request_shutdown(FF_LIFECYCLE_ADAPTER_DESTROYED);
	if (!faithfulframe_retire_session_executor(post_unload_executor) || post_unload_executor)
		return 83;
	const auto post_unload_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!post_unload_weak.expired() && std::chrono::steady_clock::now() < post_unload_deadline)
		std::this_thread::yield();
	if (!post_unload_weak.expired())
		return 84;
	std::atomic<bool> post_unload_cleanup_called{false};
	if (!faithfulframe_retire_cleanup(
		    [](void *context) {
			    static_cast<std::atomic<bool> *>(context)->store(true, std::memory_order_release);
		    },
		    &post_unload_cleanup_called))
		return 85;
	const auto post_unload_cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!post_unload_cleanup_called.load(std::memory_order_acquire) &&
	       std::chrono::steady_clock::now() < post_unload_cleanup_deadline)
		std::this_thread::yield();
	if (!post_unload_cleanup_called.load(std::memory_order_acquire))
		return 86;
	{
		faithfulframe_session_executor preview_executor(config, ff_identity{110, 210}, generations);
		ff_snapshot preview_snapshot{};
		if (!await_snapshot(
			    preview_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
			    preview_snapshot))
			return 75;
		const ff_command preview_enable{FF_COMMAND_ENABLE_AUTO_FOLLOW,
						preview_snapshot.identity,
						preview_snapshot.status_version,
						generations,
						0,
						0};
		if (!preview_executor.enqueue_command(1, preview_enable))
			return 76;
		const ff_candidate preview_candidate{41, 4001, preview_snapshot.identity, 1, generations,
						     2,  502,  {0.4f, 0.2f, 0.1f, 0.2f}};
		if (!preview_executor.enqueue_candidates(2, &preview_candidate, 1) ||
		    !await_snapshot(
			    preview_executor, [](const ff_snapshot &value) { return value.observation_version == 1; },
			    preview_snapshot))
			return 77;
		const ff_command preview_select{FF_COMMAND_SELECT_CANDIDATE,     preview_snapshot.identity,
						preview_snapshot.status_version, generations,
						preview_candidate.handle,        preview_candidate.observation_version};
		if (!preview_executor.enqueue_command(3, preview_select) ||
		    !await_snapshot(
			    preview_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_LOCKED; },
			    preview_snapshot))
			return 78;
		if (!preview_executor.enqueue_candidate_clear(4, preview_snapshot.identity, generations, 2) ||
		    !await_snapshot(
			    preview_executor,
			    [](const ff_snapshot &value) {
				    return value.observation_version == 2 && value.state == FF_STATE_LOCKED;
			    },
			    preview_snapshot))
			return 79;
		const ff_command stale_preview_click{
			FF_COMMAND_SELECT_CANDIDATE,  preview_snapshot.identity, preview_snapshot.status_version,
			preview_snapshot.generations, preview_candidate.handle,  preview_snapshot.observation_version};
		if (!preview_executor.enqueue_command(5, stale_preview_click) ||
		    !await_snapshot(
			    preview_executor,
			    [](const ff_snapshot &value) {
				    return value.command_result == FF_COMMAND_REJECTED &&
					   value.rejection_reason == FF_REJECTION_STALE_COMMAND;
			    },
			    preview_snapshot) ||
		    preview_snapshot.state != FF_STATE_LOCKED ||
		    preview_snapshot.selected_handle != preview_candidate.handle)
			return 80;
	}
	// This explicit barrier represents final process teardown after all OBS
	// source callbacks have completed. Only after it closes may later retirement
	// attempts fall back to caller-owned cleanup.
	faithfulframe_executor_reaper_shutdown();
	std::atomic<bool> late_cleanup_called{false};
	if (faithfulframe_retire_cleanup(
		    [](void *context) {
			    static_cast<std::atomic<bool> *>(context)->store(true, std::memory_order_release);
		    },
		    &late_cleanup_called) ||
	    late_cleanup_called.load(std::memory_order_acquire))
		return 72;
	auto late_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{108, 208}, generations);
	ff_snapshot late_snapshot{};
	if (!await_snapshot(
		    *late_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    late_snapshot))
		return 73;
	late_executor->request_shutdown();
	if (faithfulframe_retire_session_executor(late_executor) || !late_executor)
		return 74;
	// A caller that loses the reaper race retains ownership and performs the
	// blocking final join itself rather than leaking a worker.
	late_executor->shutdown();
	return 0;
}

int main()
{
	const int result = run_smoke();
	if (result != 0)
		std::fprintf(stderr, "faithfulframe-session-executor-smoke failed at step %d\n", result);
	return result;
}
