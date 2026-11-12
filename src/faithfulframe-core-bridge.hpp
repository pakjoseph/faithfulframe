#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "faithfulframe_ffi.h"
#include "faithfulframe-candidate-limits.hpp"

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
	uint64_t accepted_work = 0;
	uint64_t rejected_work = 0;
	uint64_t coalesced_work = 0;
	uint64_t last_queue_wait_us = 0;
	uint64_t max_queue_wait_us = 0;
	uint64_t last_processing_us = 0;
	uint64_t max_processing_us = 0;
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

	faithfulframe_session_executor(const ff_session_config &, const ff_identity &, const ff_generations &);
	~faithfulframe_session_executor();
	faithfulframe_session_executor(const faithfulframe_session_executor &) = delete;
	faithfulframe_session_executor &operator=(const faithfulframe_session_executor &) = delete;

	bool enqueue_tick(uint64_t monotonic_ms);
	bool enqueue_candidates(uint64_t monotonic_ms, const ff_candidate *candidates, size_t count);
	bool enqueue_candidate_clear(uint64_t monotonic_ms, const ff_identity &identity,
				     const ff_generations &generations, uint64_t observation_version);
	bool enqueue_command(uint64_t monotonic_ms, const ff_command &command);
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
		static constexpr size_t word_count = (sizeof(ff_snapshot) + sizeof(uint64_t) - 1) / sizeof(uint64_t);
		std::array<std::atomic<uint64_t>, word_count> words{};
		std::atomic<uint64_t> sequence{0};
		std::atomic<uint64_t> published_at_ms{0};

	public:
		void publish(const ff_snapshot &snapshot, uint64_t published_at_ms);
		bool try_read(ff_snapshot &snapshot, uint64_t *published_at_ms) const;
	};

	bool enqueue(work_item item, bool coalesce);
	void run();

	const ff_session_config config;
	const ff_identity identity;
	const ff_generations initial_generations;
	static constexpr size_t max_queued_work = 16;
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
	std::atomic<uint64_t> accepted_work{0};
	std::atomic<uint64_t> rejected_work{0};
	std::atomic<uint64_t> coalesced_work{0};
	std::atomic<uint64_t> last_queue_wait_us{0};
	std::atomic<uint64_t> max_queue_wait_us{0};
	std::atomic<uint64_t> last_processing_us{0};
	std::atomic<uint64_t> max_processing_us{0};
};

/// Transfers an executor with a pending shutdown or terminal lifecycle to the
/// shared reaper, which joins its worker.
/// The caller retains ownership if the reaper cannot accept it.
bool faithfulframe_retire_session_executor(std::shared_ptr<faithfulframe_session_executor> &executor);

/// Defers destruction that may join tracker/detector workers to the shared reaper.
/// The caller retains context ownership if the reaper cannot accept the callback.
bool faithfulframe_retire_cleanup(void (*cleanup)(void *), void *context);

/// Final process-teardown barrier. Do not call from obs_module_unload(): libobs
/// may invoke remaining source destroy callbacks after module unload returns.
/// The reaper remains available until process teardown so those callbacks can
/// transfer blocking worker joins off the OBS callback thread.
/// Starts the asynchronous teardown worker. Module loading must fail if it
/// cannot be started, because source destruction must not fall back to joins
/// on OBS callback threads.
extern "C" int faithfulframe_executor_reaper_start();
extern "C" void faithfulframe_executor_reaper_shutdown();
