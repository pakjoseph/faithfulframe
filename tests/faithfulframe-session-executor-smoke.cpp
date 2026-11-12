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
struct reaper_drain_test_gate
{
	std::mutex mutex;
	std::condition_variable condition;
	bool drain_paused = false;
	bool release_drain = false;
	bool unload_returned = false;
};

struct reaper_enqueue_test_gate
{
	std::mutex mutex;
	std::condition_variable condition;
	bool enqueue_paused = false;
	bool release_enqueue = false;
};

void pause_reaper_drain(void *context)
{
	auto *gate = static_cast<reaper_drain_test_gate *>(context);
	std::unique_lock<std::mutex> lock(gate->mutex);
	gate->drain_paused = true;
	gate->condition.notify_all();
	gate->condition.wait(lock, [gate] { return gate->release_drain; });
}

void pause_reaper_enqueue(void *context)
{
	auto *gate = static_cast<reaper_enqueue_test_gate *>(context);
	std::unique_lock<std::mutex> lock(gate->mutex);
	gate->enqueue_paused = true;
	gate->condition.notify_all();
	gate->condition.wait(lock, [gate] { return gate->release_enqueue; });
}

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

bool retire_executor_when_available(std::shared_ptr<faithfulframe_session_executor> &executor)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	do {
		if (faithfulframe_retire_session_executor(executor))
			return true;
		if (!executor)
			return false;
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

bool retirement_queue_handles_concurrent_handoffs(const ff_session_config &config, const ff_generations &generations)
{
	constexpr size_t executor_count = 12;
	constexpr size_t handoff_threads = 3;
	std::array<std::shared_ptr<faithfulframe_session_executor>, executor_count> executors{};
	std::array<std::weak_ptr<faithfulframe_session_executor>, executor_count> weak_executors{};
	std::atomic<bool> failed{false};
	for (size_t i = 0; i < executor_count; ++i) {
		executors[i] = std::make_shared<faithfulframe_session_executor>(config, ff_identity{120 + i, 220 + i},
										generations);
		weak_executors[i] = executors[i];
		if (!executors[i]->worker_available())
			return false;
		executors[i]->request_shutdown();
	}

	std::array<std::thread, handoff_threads> producers;
	for (size_t thread_index = 0; thread_index < handoff_threads; ++thread_index) {
		producers[thread_index] = std::thread([&, thread_index] {
			for (size_t i = thread_index; i < executor_count; i += handoff_threads) {
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (executors[i] && std::chrono::steady_clock::now() < deadline) {
					if (faithfulframe_retire_session_executor(executors[i]))
						break;
					std::this_thread::yield();
				}
				if (executors[i])
					failed.store(true, std::memory_order_release);
			}
		});
	}
	for (auto &producer : producers)
		producer.join();
	if (failed.load(std::memory_order_acquire))
		return false;

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	for (;;) {
		bool drained = true;
		for (const auto &weak_executor : weak_executors)
			drained = drained && weak_executor.expired();
		if (drained)
			break;
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::yield();
	}

	auto duplicate_retirement =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{140, 240}, generations);
	if (!duplicate_retirement->worker_available())
		return false;
	std::weak_ptr<faithfulframe_session_executor> duplicate_weak = duplicate_retirement;
	auto duplicate_retirement_copy = duplicate_retirement;
	duplicate_retirement->request_shutdown();
	std::barrier duplicate_start(3);
	std::atomic<bool> duplicate_failed{false};
	std::thread first_retirement([&] {
		duplicate_start.arrive_and_wait();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (duplicate_retirement && std::chrono::steady_clock::now() < deadline) {
			if (faithfulframe_retire_session_executor(duplicate_retirement))
				break;
			std::this_thread::yield();
		}
		if (duplicate_retirement)
			duplicate_failed.store(true, std::memory_order_release);
	});
	std::thread second_retirement([&] {
		duplicate_start.arrive_and_wait();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (duplicate_retirement_copy && std::chrono::steady_clock::now() < deadline) {
			if (faithfulframe_retire_session_executor(duplicate_retirement_copy))
				break;
			std::this_thread::yield();
		}
		if (duplicate_retirement_copy)
			duplicate_failed.store(true, std::memory_order_release);
	});
	duplicate_start.arrive_and_wait();
	first_retirement.join();
	second_retirement.join();
	if (duplicate_failed.load(std::memory_order_acquire))
		return false;
	const auto duplicate_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!duplicate_weak.expired() && std::chrono::steady_clock::now() < duplicate_deadline)
		std::this_thread::yield();
	return duplicate_weak.expired();
}

bool snapshot_publication_is_consistent(const ff_session_config &config, const ff_generations &generations)
{
	const ff_identity identity{150, 250};
	faithfulframe_session_executor executor(config, identity, generations);
	ff_snapshot snapshot{};
	if (!await_snapshot(executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; }, snapshot))
		return false;

	constexpr size_t reader_count = 4;
	std::barrier start(static_cast<std::ptrdiff_t>(reader_count + 1));
	std::atomic<bool> finished{false};
	std::atomic<bool> inconsistent{false};
	std::atomic<uint64_t> successful_reads{0};
	std::array<std::thread, reader_count> readers;
	for (auto &reader : readers) {
		reader = std::thread([&] {
			start.arrive_and_wait();
			uint64_t previous_version = 0;
			while (!finished.load(std::memory_order_acquire)) {
				ff_snapshot observed{};
				if (!executor.try_snapshot(observed)) {
					std::this_thread::yield();
					continue;
				}
				const bool state_reason_match =
					(observed.reason == FF_REASON_STARTUP && observed.state == FF_STATE_MANUAL) ||
					(observed.reason == FF_REASON_AWAITING_SELECTION &&
					 (observed.state == FF_STATE_MANUAL || observed.state == FF_STATE_SEARCHING)) ||
					(observed.reason == FF_REASON_OPERATOR_FREEZE &&
					 observed.state == FF_STATE_FROZEN);
				const bool crop_action_match =
					(observed.state == FF_STATE_MANUAL && observed.crop_action == FF_CROP_NONE) ||
					((observed.state == FF_STATE_SEARCHING || observed.state == FF_STATE_FROZEN) &&
					 observed.crop_action == FF_CROP_HOLD);
				const bool consistent =
					observed.identity.source == identity.source &&
					observed.identity.session == identity.session &&
					observed.generations.source == generations.source &&
					observed.generations.session == generations.session &&
					observed.generations.stream == generations.stream &&
					observed.generations.geometry == generations.geometry &&
					observed.generations.tracker == generations.tracker &&
					observed.status_version >= previous_version && !observed.closed &&
					!observed.has_selected && state_reason_match && crop_action_match &&
					observed.crop.x == 0.0f && observed.crop.y == 0.0f &&
					observed.crop.width == 1.0f && observed.crop.height == 1.0f &&
					observed.viewport.x == 0.0f && observed.viewport.y == 0.0f &&
					observed.viewport.width == 1.0f && observed.viewport.height == 1.0f;
				if (!consistent)
					inconsistent.store(true, std::memory_order_release);
				previous_version = observed.status_version;
				successful_reads.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	start.arrive_and_wait();

	uint64_t at = 1;
	for (size_t i = 0; i < 150; ++i) {
		const uint32_t kinds[] = {FF_COMMAND_ENABLE_AUTO_FOLLOW, FF_COMMAND_FREEZE, FF_COMMAND_RESUME};
		for (const uint32_t kind : kinds) {
			const uint64_t previous_version = snapshot.status_version;
			const ff_command command{
				kind, snapshot.identity, snapshot.status_version, snapshot.generations, 0, 0};
			if (!executor.enqueue_command(at++, command) ||
			    !await_snapshot(
				    executor,
				    [previous_version](const ff_snapshot &value) {
					    return value.status_version > previous_version &&
						   value.command_result == FF_COMMAND_APPLIED;
				    },
				    snapshot)) {
				finished.store(true, std::memory_order_release);
				for (auto &reader : readers)
					reader.join();
				executor.shutdown();
				return false;
			}
		}
	}
	finished.store(true, std::memory_order_release);
	for (auto &reader : readers)
		reader.join();
	executor.shutdown();
	return !inconsistent.load(std::memory_order_acquire) &&
	       successful_reads.load(std::memory_order_relaxed) > reader_count;
}

bool retirement_queue_contention_is_nonblocking(const ff_session_config &config, const ff_generations &generations)
{
	auto paused_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{150, 250}, generations);
	auto contended_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{151, 251}, generations);
	ff_snapshot paused_snapshot{};
	ff_snapshot contended_snapshot{};
	if (!await_snapshot(
		    *paused_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    paused_snapshot) ||
	    !await_snapshot(
		    *contended_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    contended_snapshot))
		return false;
	paused_executor->request_shutdown();
	contended_executor->request_shutdown();
	std::weak_ptr<faithfulframe_session_executor> paused_weak = paused_executor;
	std::weak_ptr<faithfulframe_session_executor> contended_weak = contended_executor;
	reaper_enqueue_test_gate gate;
	faithfulframe_test_set_reaper_enqueue_hook(pause_reaper_enqueue, &gate);
	std::atomic<bool> paused_accepted{false};
	std::thread paused_handoff([&] {
		paused_accepted.store(faithfulframe_retire_session_executor(paused_executor),
				      std::memory_order_release);
	});
	bool paused_inside_enqueue = false;
	{
		std::unique_lock<std::mutex> lock(gate.mutex);
		paused_inside_enqueue =
			gate.condition.wait_for(lock, std::chrono::seconds(2), [&] { return gate.enqueue_paused; });
	}
	std::mutex completion_mutex;
	std::condition_variable completion_condition;
	std::atomic<bool> contended_finished{false};
	std::atomic<bool> contended_accepted{true};
	std::thread contended_handoff([&] {
		contended_accepted.store(faithfulframe_retire_session_executor(contended_executor),
					 std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(completion_mutex);
			contended_finished.store(true, std::memory_order_release);
		}
		completion_condition.notify_all();
	});
	bool contended_returned_while_locked = false;
	{
		std::unique_lock<std::mutex> lock(completion_mutex);
		contended_returned_while_locked =
			completion_condition.wait_for(lock, std::chrono::milliseconds(250), [&] {
				return contended_finished.load(std::memory_order_acquire);
			});
	}
	{
		std::lock_guard<std::mutex> lock(gate.mutex);
		gate.release_enqueue = true;
	}
	gate.condition.notify_all();
	paused_handoff.join();
	contended_handoff.join();
	faithfulframe_test_set_reaper_enqueue_hook(nullptr, nullptr);
	if (!paused_inside_enqueue || !contended_returned_while_locked ||
	    !paused_accepted.load(std::memory_order_acquire) || paused_executor ||
	    contended_accepted.load(std::memory_order_acquire) || !contended_executor)
		return false;
	if (!retire_executor_when_available(contended_executor) || contended_executor)
		return false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while ((!paused_weak.expired() || !contended_weak.expired()) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	return paused_weak.expired() && contended_weak.expired();
}
} // namespace

int run_smoke()
{
	if (faithfulframe_teardown_register_instance() != 0)
		return 92;
	if (faithfulframe_executor_reaper_start() != 1)
		return 1;
	const ff_session_config config{5000, 500,  8,     3840,  2160,  1920,  1080, 0.45f,
				       1.0f, 4.0f, 0.25f, 0.08f, 0.08f, 0.05f, 0.25f};
	if (!retirement_queue_contention_is_nonblocking(config, ff_generations{1, 1, 1, 1, 1}))
		return 110;
	if (faithfulframe_teardown_register_instance() != 1)
		return 94;
	faithfulframe_teardown_instance_created();
	auto last_source_executor = std::make_shared<faithfulframe_session_executor>(config, ff_identity{109, 209},
										     ff_generations{1, 1, 1, 1, 1});
	ff_snapshot last_source_snapshot{};
	if (!await_snapshot(
		    *last_source_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    last_source_snapshot))
		return 95;
	std::weak_ptr<faithfulframe_session_executor> last_source_weak = last_source_executor;
	last_source_executor->request_shutdown(FF_LIFECYCLE_ADAPTER_DESTROYED);
	if (!faithfulframe_retire_session_executor(last_source_executor) || last_source_executor)
		return 96;
	faithfulframe_teardown_instance_destroyed();
	if (!last_source_weak.expired() || faithfulframe_teardown_register_instance() != 1)
		return 97;
	// Failed creates only release their reservation. They do not turn into
	// active source instances and cannot defer the destruction-thread drain.
	faithfulframe_teardown_instance_creation_failed();
	if (faithfulframe_teardown_register_instance() != 1 || faithfulframe_teardown_register_instance() != 1)
		return 87;
	faithfulframe_teardown_instance_created();
	faithfulframe_teardown_instance_created();
	if (!configuration_revision_guard_is_race_safe())
		return 23;
	if (!snapshot_publication_is_consistent(config, ff_generations{1, 1, 1, 1, 1}))
		return 93;
	if (!retirement_queue_handles_concurrent_handoffs(config, ff_generations{1, 1, 1, 1, 1}))
		return 91;
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
	if (completed_diagnostics.queue_depth != 0 || completed_diagnostics.oldest_queue_age_us != 0 ||
	    completed_diagnostics.queue_high_water != 1 || completed_diagnostics.accepted_work != 5 ||
	    completed_diagnostics.rejected_work != 0 || completed_diagnostics.coalesced_work != 0 ||
	    completed_diagnostics.last_queue_wait_us > 10'000'000)
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
	if (teardown_diagnostics.queue_depth != 0 || teardown_diagnostics.oldest_queue_age_us != 0 ||
	    teardown_diagnostics.accepted_work != 3 || teardown_diagnostics.rejected_work != 4 ||
	    teardown_diagnostics.coalesced_work != 0 || teardown_diagnostics.queue_high_water > 3 ||
	    teardown_diagnostics.last_queue_wait_us > 10'000'000)
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
	const auto unload_drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!unload_weak.expired() && std::chrono::steady_clock::now() < unload_drain_deadline)
		std::this_thread::yield();
	if (!unload_weak.expired())
		return 63;
	// OBS may destroy plugin sources after obs_module_unload returns. Unload
	// keeps the reaper alive while registered source instances remain.
	faithfulframe_teardown_begin_module_unload();
	if (faithfulframe_teardown_register_instance() != 0)
		return 88;
	auto post_unload_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{111, 211}, generations);
	ff_snapshot post_unload_snapshot{};
	if (!await_snapshot(
		    *post_unload_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    post_unload_snapshot))
		return 82;
	std::weak_ptr<faithfulframe_session_executor> post_unload_weak = post_unload_executor;
	post_unload_executor->request_shutdown(FF_LIFECYCLE_ADAPTER_DESTROYED);
	if (!retire_executor_when_available(post_unload_executor) || post_unload_executor)
		return 83;
	const auto post_unload_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!post_unload_weak.expired() && std::chrono::steady_clock::now() < post_unload_deadline)
		std::this_thread::yield();
	if (!post_unload_weak.expired())
		return 84;
	{
		ff_session_config preview_config = config;
		// Let the command notice expire before the late-candidate event without
		// making the selected evidence stale during this isolated version test.
		preview_config.max_evidence_age_ms = 10'000;
		faithfulframe_session_executor preview_executor(preview_config, ff_identity{110, 210}, generations);
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
		const ff_snapshot after_stale_click = preview_snapshot;
		const ff_candidate late_candidate{42,   4002, preview_snapshot.identity, 2, generations,
						  2006, 2506, {0.6f, 0.2f, 0.1f, 0.2f}};
		if (!preview_executor.enqueue_candidates(2006, &late_candidate, 1))
			return 91;
		// Candidate-event rejections are not retained as operator notices. Wait
		// for this event's publication timestamp, then verify its policy no-op.
		uint64_t candidate_published_at = 0;
		const auto candidate_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		do {
			preview_executor.try_snapshot(preview_snapshot, &candidate_published_at);
			if (candidate_published_at >= 2006)
				break;
			std::this_thread::yield();
		} while (std::chrono::steady_clock::now() < candidate_deadline);
		if (candidate_published_at < 2006)
			return 92;
		if (preview_snapshot.state != after_stale_click.state ||
		    preview_snapshot.selected_handle != after_stale_click.selected_handle ||
		    preview_snapshot.status_version != after_stale_click.status_version ||
		    preview_snapshot.observation_version != after_stale_click.observation_version ||
		    preview_snapshot.crop.x != after_stale_click.crop.x ||
		    preview_snapshot.crop.y != after_stale_click.crop.y ||
		    preview_snapshot.crop.width != after_stale_click.crop.width ||
		    preview_snapshot.crop.height != after_stale_click.crop.height)
			return 93;
	}
	// The first destroy callback leaves cleanup available for the second active
	// source. The last callback drains and closes the reaper.
	faithfulframe_teardown_instance_destroyed();
	auto final_pending_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{112, 212}, generations);
	ff_snapshot final_pending_snapshot{};
	if (!await_snapshot(
		    *final_pending_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    final_pending_snapshot))
		return 89;
	std::weak_ptr<faithfulframe_session_executor> final_pending_weak = final_pending_executor;
	final_pending_executor->request_shutdown();
	if (!faithfulframe_retire_session_executor(final_pending_executor) || final_pending_executor)
		return 90;
	faithfulframe_teardown_instance_destroyed();
	if (!final_pending_weak.expired())
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
	if (faithfulframe_executor_reaper_start() != 1 || faithfulframe_teardown_register_instance() != 1)
		return 98;
	faithfulframe_teardown_begin_module_unload();
	if (faithfulframe_teardown_register_instance() != 0)
		return 99;
	faithfulframe_teardown_instance_creation_failed();
	if (faithfulframe_teardown_register_instance() != 0)
		return 100;

	// A create already in flight at module unload may still succeed. Keep the
	// reaper alive for it, then close it from that instance's destruction path.
	if (faithfulframe_executor_reaper_start() != 1 || faithfulframe_teardown_register_instance() != 1)
		return 101;
	faithfulframe_teardown_begin_module_unload();
	faithfulframe_teardown_instance_created();
	auto in_flight_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{113, 213}, generations);
	ff_snapshot in_flight_snapshot{};
	if (!await_snapshot(
		    *in_flight_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    in_flight_snapshot))
		return 102;
	std::weak_ptr<faithfulframe_session_executor> in_flight_weak = in_flight_executor;
	in_flight_executor->request_shutdown(FF_LIFECYCLE_ADAPTER_DESTROYED);
	if (!faithfulframe_retire_session_executor(in_flight_executor) || in_flight_executor)
		return 103;
	faithfulframe_teardown_instance_destroyed();
	if (!in_flight_weak.expired() || faithfulframe_teardown_register_instance() != 0)
		return 104;

	// Hold the last-source drain after all joins complete but before its final
	// lifecycle check. Module unload must observe the drain barrier and return
	// without attempting to join the reaper on its caller thread.
	if (faithfulframe_executor_reaper_start() != 1 || faithfulframe_teardown_register_instance() != 1)
		return 105;
	faithfulframe_teardown_instance_created();
	auto racing_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{114, 214}, generations);
	ff_snapshot racing_snapshot{};
	if (!await_snapshot(
		    *racing_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    racing_snapshot))
		return 106;
	std::weak_ptr<faithfulframe_session_executor> racing_weak = racing_executor;
	racing_executor->request_shutdown(FF_LIFECYCLE_ADAPTER_DESTROYED);
	if (!faithfulframe_retire_session_executor(racing_executor) || racing_executor)
		return 107;
	reaper_drain_test_gate gate;
	faithfulframe_test_set_reaper_drain_hook(pause_reaper_drain, &gate);
	std::thread destroy_thread([] { faithfulframe_teardown_instance_destroyed(); });
	bool drain_paused = false;
	{
		std::unique_lock<std::mutex> lock(gate.mutex);
		drain_paused =
			gate.condition.wait_for(lock, std::chrono::seconds(2), [&gate] { return gate.drain_paused; });
	}
	std::thread unload_thread([&gate] {
		faithfulframe_teardown_begin_module_unload();
		{
			std::lock_guard<std::mutex> lock(gate.mutex);
			gate.unload_returned = true;
		}
		gate.condition.notify_all();
	});
	bool unload_returned_before_drain_release = false;
	{
		std::unique_lock<std::mutex> lock(gate.mutex);
		unload_returned_before_drain_release = gate.condition.wait_for(lock, std::chrono::seconds(2), [&gate] {
			return gate.unload_returned;
		});
	}
	std::atomic<bool> registration_finished{false};
	std::atomic<int> registration_result{-1};
	std::thread registration_thread([&] {
		const int result = faithfulframe_teardown_register_instance();
		if (result == 1)
			faithfulframe_teardown_instance_creation_failed();
		{
			std::lock_guard<std::mutex> lock(gate.mutex);
			registration_result.store(result, std::memory_order_release);
			registration_finished.store(true, std::memory_order_release);
		}
		gate.condition.notify_all();
	});
	bool registration_finished_during_drain = false;
	{
		std::unique_lock<std::mutex> lock(gate.mutex);
		registration_finished_during_drain = gate.condition.wait_for(lock, std::chrono::seconds(2), [&] {
			return registration_finished.load(std::memory_order_acquire);
		});
	}
	auto contended_executor =
		std::make_shared<faithfulframe_session_executor>(config, ff_identity{115, 215}, generations);
	ff_snapshot contended_snapshot{};
	if (!await_snapshot(
		    *contended_executor, [](const ff_snapshot &value) { return value.state == FF_STATE_MANUAL; },
		    contended_snapshot))
		return 109;
	contended_executor->request_shutdown(FF_LIFECYCLE_ADAPTER_DESTROYED);
	std::mutex handoff_mutex;
	std::condition_variable handoff_condition;
	std::atomic<bool> handoff_started{false};
	std::atomic<bool> handoff_finished{false};
	std::atomic<bool> handoff_accepted{true};
	std::weak_ptr<faithfulframe_session_executor> contended_weak = contended_executor;
	std::thread handoff_thread([&] {
		handoff_started.store(true, std::memory_order_release);
		handoff_accepted.store(faithfulframe_retire_session_executor(contended_executor),
				       std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(handoff_mutex);
			handoff_finished.store(true, std::memory_order_release);
		}
		handoff_condition.notify_all();
	});
	while (!handoff_started.load(std::memory_order_acquire))
		std::this_thread::yield();
	bool handoff_finished_during_drain = false;
	{
		std::unique_lock<std::mutex> lock(handoff_mutex);
		handoff_finished_during_drain = handoff_condition.wait_for(lock, std::chrono::seconds(2), [&] {
			return handoff_finished.load(std::memory_order_acquire);
		});
	}
	{
		std::unique_lock<std::mutex> lock(gate.mutex);
		gate.release_drain = true;
	}
	gate.condition.notify_all();
	destroy_thread.join();
	unload_thread.join();
	registration_thread.join();
	handoff_thread.join();
	faithfulframe_test_set_reaper_drain_hook(nullptr, nullptr);
	if (!drain_paused || !unload_returned_before_drain_release || !registration_finished_during_drain ||
	    registration_result.load(std::memory_order_acquire) != 0 || !racing_weak.expired() ||
	    !handoff_finished_during_drain || !handoff_accepted.load(std::memory_order_acquire) || contended_executor ||
	    !contended_weak.expired()) {
		std::fprintf(stderr,
			     "drain race: paused=%d unload=%d registration=%d registration_result=%d racing_reaped=%d "
			     "handoff=%d accepted=%d owner_empty=%d contended_reaped=%d\n",
			     drain_paused, unload_returned_before_drain_release, registration_finished_during_drain,
			     registration_result.load(std::memory_order_acquire), racing_weak.expired(),
			     handoff_finished_during_drain, handoff_accepted.load(std::memory_order_acquire),
			     !contended_executor, contended_weak.expired());
		return 108;
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
