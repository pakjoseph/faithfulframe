#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "faithfulframe_ffi.h"
#include "faithfulframe-candidate-limits.hpp"

inline constexpr size_t faithfulframe_snapshot_word_count =
	(sizeof(ff_snapshot) + sizeof(uint64_t) - 1) / sizeof(uint64_t);

/// Copy only defined FFI fields into a zero-initialized record before treating
/// its object representation as atomic words. Rust repr(C) padding is not part
/// of the ABI and may otherwise contain indeterminate bytes.
inline std::array<uint64_t, faithfulframe_snapshot_word_count>
faithfulframe_snapshot_words(const ff_snapshot &snapshot) noexcept
{
	ff_snapshot canonical{};
	canonical.state = snapshot.state;
	canonical.reason = snapshot.reason;
	canonical.crop_action = snapshot.crop_action;
	canonical.identity = ff_identity{snapshot.identity.source, snapshot.identity.session};
	canonical.status_version = snapshot.status_version;
	canonical.observation_version = snapshot.observation_version;
	canonical.selected_handle = snapshot.selected_handle;
	canonical.loss_deadline_ms = snapshot.loss_deadline_ms;
	canonical.has_selected = snapshot.has_selected;
	canonical.has_loss_deadline = snapshot.has_loss_deadline;
	canonical.closed = snapshot.closed;
	canonical.command_result = snapshot.command_result;
	canonical.rejection_reason = snapshot.rejection_reason;
	canonical.crop = ff_rect{snapshot.crop.x, snapshot.crop.y, snapshot.crop.width, snapshot.crop.height};
	canonical.viewport =
		ff_rect{snapshot.viewport.x, snapshot.viewport.y, snapshot.viewport.width, snapshot.viewport.height};
	canonical.generations = ff_generations{snapshot.generations.source, snapshot.generations.session,
					       snapshot.generations.stream, snapshot.generations.geometry,
					       snapshot.generations.tracker};
	std::array<uint64_t, faithfulframe_snapshot_word_count> words{};
	std::memcpy(words.data(), &canonical, sizeof(canonical));
	return words;
}

class faithfulframe_executor_reaper;

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
	ff_result clear_candidates(uint64_t monotonic_ms, const ff_identity &identity,
				   const ff_generations &generations, uint64_t observation_version,
				   ff_snapshot &output);
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

struct ff_executor_diagnostics
{
	uint64_t queue_depth = 0;
	uint64_t queue_high_water = 0;
	uint64_t oldest_queue_age_us = 0; // Lock-free sample; zero when the queue is empty.
	uint64_t accepted_work = 0;
	uint64_t rejected_work = 0;
	uint64_t coalesced_work = 0;
	uint64_t last_queue_wait_us = 0;
	uint64_t max_queue_wait_us = 0;
	uint64_t last_processing_us = 0;
	uint64_t max_processing_us = 0;
};

/// Intrusive single-use callback executed by the module reaper. The owner must
/// keep the node alive until enqueue succeeds and then relinquish ownership.
/// Callbacks run outside the reaper lock and are drained before module unload.
struct faithfulframe_reap_task
{
	faithfulframe_reap_task *next = nullptr;
	void (*callback)(void *) = nullptr;
	void *context = nullptr;
	void (*wake_reaper)(void *) = nullptr;
	void *wake_context = nullptr;
	// Cleanup nodes stay queued but dormant until their owner has relinquished them.
	std::atomic<bool> ready{false};
	bool queued = false;
};

/// Derives the exact next generation vector accepted by the Rust lifecycle
/// policy. All C++ lifecycle producers use this shared translation.
bool faithfulframe_next_lifecycle_generations(uint32_t kind, const ff_generations &current, ff_generations &next);

/// Owns one Rust session on a bounded background executor. OBS callbacks only
/// enqueue copied values; rendering reads a version-consistent snapshot without
/// waiting for session policy or detection work.
class faithfulframe_session_executor {
public:
	static constexpr size_t max_candidates = faithfulframe_max_candidates;
	static constexpr size_t max_queued_work = 16;
	// Keep capacity for operator commands and authoritative lifecycle events
	// while detector observations or frame ticks are backlogged.
	static constexpr size_t reserved_control_work = 4;
	static constexpr size_t max_queued_background_work = max_queued_work - reserved_control_work;

	faithfulframe_session_executor(const ff_session_config &, const ff_identity &, const ff_generations &);
	~faithfulframe_session_executor();
	faithfulframe_session_executor(const faithfulframe_session_executor &) = delete;
	faithfulframe_session_executor &operator=(const faithfulframe_session_executor &) = delete;

	bool enqueue_tick(uint64_t monotonic_ms);
	bool enqueue_candidates(uint64_t monotonic_ms, const ff_candidate *candidates, size_t count);
	bool enqueue_candidate_clear(uint64_t monotonic_ms, const ff_identity &identity,
				     const ff_generations &generations, uint64_t observation_version);
	bool enqueue_command(uint64_t monotonic_ms, const ff_command &command, bool *queue_full = nullptr);
	bool enqueue_lifecycle(uint64_t monotonic_ms, const ff_lifecycle &lifecycle);
	/// True while the worker can accept work; false after startup failure or shutdown.
	bool worker_available() const
	{
		return worker_started.load(std::memory_order_acquire) &&
		       !closing_requested.load(std::memory_order_acquire);
	}
	bool try_snapshot(ff_snapshot &output, uint64_t *published_at_ms = nullptr) const;
	ff_executor_diagnostics diagnostics() const;
	const ff_identity &binding_identity() const { return identity; }
	/// Closes the queue and schedules terminal lifecycle work without waiting.
	/// Safe for OBS callbacks, including render/tick paths.
	void request_shutdown(uint32_t terminal_lifecycle_kind = FF_LIFECYCLE_ADAPTER_DESTROYED,
			      uint32_t source_width = 0, uint32_t source_height = 0);
	/// Blocking: call only from a controlled teardown owner after adapter
	/// callbacks are quiesced; never join from render, tick, or Qt UI callbacks.
	void shutdown(uint32_t terminal_lifecycle_kind = FF_LIFECYCLE_ADAPTER_DESTROYED, uint32_t source_width = 0,
		      uint32_t source_height = 0);
	/// Joins a worker after shutdown has already been requested or a terminal
	/// lifecycle event has been queued. Never call on OBS render/tick or Qt UI.
	void join();

private:
	friend class faithfulframe_executor_reaper;
	friend bool faithfulframe_retire_session_executor(std::shared_ptr<faithfulframe_session_executor> &executor,
							  bool wait_for_reaper);

	enum class work_kind : uint8_t { tick, candidates, command, lifecycle, stop };
	struct work_item
	{
		work_kind kind = work_kind::tick;
		uint64_t monotonic_ms = 0;
		std::chrono::steady_clock::time_point enqueued_at{};
		size_t candidate_count = 0;
		uint64_t observation_version = 0;
		ff_identity candidate_identity{};
		ff_generations candidate_generations{};
		std::array<ff_candidate, max_candidates> candidates{};
		ff_command command{};
		ff_lifecycle lifecycle{};
	};

	class snapshot_publisher {
		std::array<std::atomic<uint64_t>, faithfulframe_snapshot_word_count> words{};
		std::atomic<uint64_t> sequence{0};
		std::atomic<uint64_t> published_at_ms{0};

	public:
		void publish(const ff_snapshot &snapshot, uint64_t published_at_ms);
		bool try_read(ff_snapshot &snapshot, uint64_t *published_at_ms) const;
	};

	bool enqueue(work_item item, bool coalesce, bool *queue_full = nullptr);
	void update_oldest_queue_timestamp_locked();
	void run();

	const ff_session_config config;
	const ff_identity identity;
	const ff_generations initial_generations;
	// Pin the module reaper for the executor lifetime. Source teardown can hand
	// off without consulting a global mutex, and unload cannot reclaim the
	// reaper while an adapter still owns an executor.
	const std::shared_ptr<faithfulframe_executor_reaper> reaper;
	std::array<work_item, max_queued_work + 1> queue{};
	size_t queue_head = 0;
	size_t queue_size = 0;
	uint64_t last_submitted_ms = 0;
	mutable std::mutex queue_mutex;
	std::mutex shutdown_request_mutex;
	std::mutex shutdown_mutex;
	std::condition_variable queue_condition;
	bool closing = false;
	std::atomic<bool> closing_requested{false};
	std::thread worker;
	std::atomic<bool> worker_started{false};
	snapshot_publisher published;
	std::atomic<uint64_t> queue_depth{0};
	std::atomic<uint64_t> queue_high_water{0};
	std::atomic<uint64_t> oldest_queued_at_ns{0};
	std::atomic<uint64_t> accepted_work{0};
	std::atomic<uint64_t> rejected_work{0};
	std::atomic<uint64_t> coalesced_work{0};
	std::atomic<uint64_t> last_queue_wait_us{0};
	std::atomic<uint64_t> max_queue_wait_us{0};
	std::atomic<uint64_t> last_processing_us{0};
	std::atomic<uint64_t> max_processing_us{0};

	// The module-wide reaper links pending joins through these fields. Keeping
	// an owning reference in the node makes handoff allocation-free and keeps
	// the executor alive until the reaper has taken responsibility for it.
	std::shared_ptr<faithfulframe_session_executor> retirement_keepalive;
	bool retirement_queued = false;
	faithfulframe_reap_task reap_task{};
};

/// Allocation-free handoff of a stopped executor to its pinned module reaper.
/// By default this is nonblocking for OBS callbacks; controlled teardown may
/// wait briefly for the reaper queue lock. False leaves ownership with caller.
bool faithfulframe_retire_session_executor(std::shared_ptr<faithfulframe_session_executor> &executor,
					   bool wait_for_reaper = false);
/// Enqueue a single-use cleanup task without making it executable. The caller
/// must release its ownership, then activate the task before its lifetime ends.
/// By default returns false without taking ownership if the queue is contended
/// or closed. Controlled teardown may wait briefly for the queue lock.
bool faithfulframe_defer_reap_task(faithfulframe_reap_task *task, void (*callback)(void *), void *context,
				   bool wait_for_reaper = false);
/// Publish the ownership transfer and allow the reaper to execute the task.
void faithfulframe_activate_reap_task(faithfulframe_reap_task *task);
std::shared_ptr<faithfulframe_executor_reaper> faithfulframe_acquire_executor_reaper();

/// Starts the asynchronous session-join worker. Module loading must fail if it
/// cannot be started, so nonblocking OBS callbacks can transfer joins safely.
extern "C" int faithfulframe_executor_reaper_start();
/// Drains and closes the worker after all registered source-instance cleanup
/// has been handed off.
extern "C" void faithfulframe_executor_reaper_shutdown();
/// Reserve before a source/filter create; commit successful creation or cancel
/// the reservation on failure. Live instances keep the reaper available across
/// obs_module_unload until their destroy callbacks finish.
extern "C" int faithfulframe_teardown_register_instance();
extern "C" void faithfulframe_teardown_instance_created();
extern "C" void faithfulframe_teardown_instance_destroyed();
extern "C" void faithfulframe_teardown_instance_creation_failed();
extern "C" void faithfulframe_teardown_begin_module_unload();
#ifdef FAITHFULFRAME_TEST_REAPER_JOIN_HOOK
extern "C" void faithfulframe_test_set_reaper_join_hook(void (*hook)(void *), void *context);
#endif
#ifdef FAITHFULFRAME_TEST_REAPER_ENQUEUE_HOOK
extern "C" void faithfulframe_test_set_reaper_enqueue_hook(void (*hook)(void *), void *context);
#endif
#ifdef FAITHFULFRAME_TEST_REAPER_QUEUE_LOCK_HOOK
extern "C" void faithfulframe_test_set_reaper_queue_lock_hook(void (*hook)(void *), void *context);
#endif
#ifdef FAITHFULFRAME_TEST_REAPER_WAIT_HOOK
extern "C" void faithfulframe_test_set_reaper_wait_hook(void (*hook)(void *), void *context);
#endif
#ifdef FAITHFULFRAME_TEST_SESSION_WORK_HOOK
extern "C" void faithfulframe_test_set_session_work_hook(void (*hook)(void *), void *context);
#endif
