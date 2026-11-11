#include "faithfulframe-core-bridge.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
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
} // namespace

int run_smoke()
{
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
	    completed_diagnostics.coalesced_work != 0)
		return 15;

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
	    teardown_diagnostics.queue_high_water > 3)
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
	return 0;
}

int main()
{
	const int result = run_smoke();
	if (result != 0)
		std::fprintf(stderr, "faithfulframe-session-executor-smoke failed at step %d\n", result);
	return result;
}
