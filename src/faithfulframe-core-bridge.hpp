#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

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

/// Owns one Rust session on a bounded background executor. OBS callbacks only
/// enqueue copied values; rendering reads a version-consistent snapshot without
/// waiting for session policy or detection work.
class faithfulframe_session_executor {
public:
	static constexpr size_t max_candidates = 8;

	faithfulframe_session_executor(const ff_session_config &, const ff_identity &, const ff_generations &);
	~faithfulframe_session_executor();
	faithfulframe_session_executor(const faithfulframe_session_executor &) = delete;
	faithfulframe_session_executor &operator=(const faithfulframe_session_executor &) = delete;

	bool enqueue_tick(uint64_t monotonic_ms);
	bool enqueue_candidates(uint64_t monotonic_ms, const ff_candidate *candidates, size_t count);
	bool enqueue_command(uint64_t monotonic_ms, const ff_command &command);
	bool enqueue_lifecycle(uint64_t monotonic_ms, const ff_lifecycle &lifecycle);
	bool try_snapshot(ff_snapshot &output, uint64_t *published_at_ms = nullptr) const;
	const ff_identity &binding_identity() const { return identity; }
	void shutdown(uint32_t terminal_lifecycle_kind = FF_LIFECYCLE_ADAPTER_DESTROYED, uint32_t source_width = 0,
		      uint32_t source_height = 0);

private:
	enum class work_kind : uint8_t { tick, candidates, command, lifecycle, stop };
	struct work_item
	{
		work_kind kind = work_kind::tick;
		uint64_t monotonic_ms = 0;
		size_t candidate_count = 0;
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

	bool enqueue(const work_item &item, bool coalesce);
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
	std::mutex shutdown_mutex;
	std::condition_variable queue_condition;
	bool closing = false;
	std::atomic<bool> closing_requested{false};
	std::thread worker;
	std::atomic<bool> worker_started{false};
	snapshot_publisher published;
};
