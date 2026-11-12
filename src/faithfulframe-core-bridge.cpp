#include "faithfulframe-core-bridge.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

static void update_max(std::atomic<uint64_t> &maximum, uint64_t value)
{
	uint64_t current = maximum.load(std::memory_order_relaxed);
	while (current < value &&
	       !maximum.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
	}
}

static uint64_t elapsed_microseconds(std::chrono::steady_clock::time_point start)
{
	const auto elapsed =
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	return elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
}

void faithfulframe_core_bridge::deleter::operator()(ff_session *session) const
{
	ff_session_destroy(session);
}

faithfulframe_core_bridge::faithfulframe_core_bridge(const ff_session_config &config, const ff_identity &identity,
						     const ff_generations &generations, ff_result &result)
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

ff_result faithfulframe_core_bridge::publish_candidate(uint64_t monotonic_ms, const ff_candidate &candidate,
						       ff_snapshot &output)
{
	if (!session)
		return FF_INVALID_ARGUMENT;
	return ff_session_publish_candidate(session.get(), monotonic_ms, &candidate, &output);
}

ff_result faithfulframe_core_bridge::clear_candidates(uint64_t monotonic_ms, const ff_identity &identity,
						      const ff_generations &generations, uint64_t observation_version,
						      ff_snapshot &output)
{
	if (!session)
		return FF_INVALID_ARGUMENT;
	return ff_session_clear_candidates(session.get(), monotonic_ms, &identity, &generations, observation_version,
					   &output);
}

ff_result faithfulframe_core_bridge::apply_command(uint64_t monotonic_ms, const ff_command &command,
						   ff_snapshot &output)
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

static_assert(std::atomic<uint64_t>::is_always_lock_free, "OBS render snapshots require lock-free 64-bit atomics");

bool faithfulframe_next_lifecycle_generations(uint32_t kind, const ff_generations &current, ff_generations &next)
{
	next = current;
	auto increment = [](uint64_t &value) {
		if (value == std::numeric_limits<uint64_t>::max())
			return false;
		++value;
		return true;
	};
	switch (kind) {
	case FF_LIFECYCLE_SCENE_INACTIVE:
	case FF_LIFECYCLE_SCENE_ACTIVE:
		return increment(next.session) && increment(next.tracker);
	case FF_LIFECYCLE_CAMERA_DISCONNECTED:
	case FF_LIFECYCLE_CAMERA_RESTORED:
		return increment(next.stream) && increment(next.tracker);
	case FF_LIFECYCLE_SOURCE_REBOUND:
		return increment(next.source) && increment(next.session) && increment(next.stream) &&
		       increment(next.geometry) && increment(next.tracker);
	case FF_LIFECYCLE_GEOMETRY_CHANGED:
		return increment(next.stream) && increment(next.geometry) && increment(next.tracker);
	case FF_LIFECYCLE_TRACKER_CHANGED:
		return increment(next.tracker);
	case FF_LIFECYCLE_FRAMING_CONFIG_CHANGED:
		return increment(next.session) && increment(next.geometry) && increment(next.tracker);
	case FF_LIFECYCLE_CANDIDATE_POLICY_CHANGED:
	case FF_LIFECYCLE_ADAPTER_DESTROYED:
		return increment(next.session) && increment(next.tracker);
	case FF_LIFECYCLE_SOURCE_DESTROYED:
	case FF_LIFECYCLE_OBS_SHUTDOWN:
		return increment(next.source) && increment(next.session) && increment(next.stream) &&
		       increment(next.tracker);
	default:
		return false;
	}
}

faithfulframe_session_executor::faithfulframe_session_executor(const ff_session_config &config_,
							       const ff_identity &identity_,
							       const ff_generations &generations)
	: config(config_), identity(identity_), initial_generations(generations)
{
	worker_started.store(true, std::memory_order_release);
	try {
		worker = std::thread(&faithfulframe_session_executor::run, this);
	} catch (...) {
		worker_started.store(false, std::memory_order_release);
	}
}

faithfulframe_session_executor::~faithfulframe_session_executor()
{
	shutdown();
}

bool faithfulframe_session_executor::enqueue_tick(uint64_t monotonic_ms)
{
	work_item item;
	item.kind = work_kind::tick;
	item.monotonic_ms = monotonic_ms;
	return enqueue(item, true);
}

bool faithfulframe_session_executor::enqueue_candidates(uint64_t monotonic_ms, const ff_candidate *candidates,
							size_t count)
{
	if ((count && !candidates) || count > max_candidates) {
		rejected_work.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	if (!count) {
		rejected_work.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const uint64_t batch_version = candidates[0].observation_version;
	const ff_identity identity = candidates[0].identity;
	const ff_generations generations = candidates[0].generations;
	if (!batch_version || !identity.source || !identity.session) {
		rejected_work.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	for (size_t i = 1; i < count; ++i) {
		const ff_candidate &candidate = candidates[i];
		if (candidate.observation_version != batch_version || candidate.identity.source != identity.source ||
		    candidate.identity.session != identity.session ||
		    candidate.generations.source != generations.source ||
		    candidate.generations.session != generations.session ||
		    candidate.generations.stream != generations.stream ||
		    candidate.generations.geometry != generations.geometry ||
		    candidate.generations.tracker != generations.tracker) {
			rejected_work.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}
	work_item item;
	item.kind = work_kind::candidates;
	item.monotonic_ms = monotonic_ms;
	item.candidate_count = count;
	item.observation_version = batch_version;
	item.candidate_identity = identity;
	item.candidate_generations = generations;
	for (size_t i = 0; i < count; ++i)
		item.candidates[i] = candidates[i];
	return enqueue(item, true);
}

bool faithfulframe_session_executor::enqueue_candidate_clear(uint64_t monotonic_ms, const ff_identity &identity,
							     const ff_generations &generations,
							     uint64_t observation_version)
{
	if (!identity.source || !identity.session || !observation_version) {
		rejected_work.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	work_item item;
	item.kind = work_kind::candidates;
	item.monotonic_ms = monotonic_ms;
	item.observation_version = observation_version;
	item.candidate_identity = identity;
	item.candidate_generations = generations;
	return enqueue(item, true);
}

bool faithfulframe_session_executor::enqueue_command(uint64_t monotonic_ms, const ff_command &command)
{
	work_item item;
	item.kind = work_kind::command;
	item.monotonic_ms = monotonic_ms;
	item.command = command;
	return enqueue(item, false);
}

bool faithfulframe_session_executor::enqueue_lifecycle(uint64_t monotonic_ms, const ff_lifecycle &lifecycle)
{
	work_item item;
	item.kind = work_kind::lifecycle;
	item.monotonic_ms = monotonic_ms;
	item.lifecycle = lifecycle;
	return enqueue(item, false);
}

bool faithfulframe_session_executor::enqueue(work_item item, bool coalesce)
{
	if (!worker_started.load(std::memory_order_acquire)) {
		rejected_work.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	item.enqueued_at = std::chrono::steady_clock::now();
	// Queue bookkeeping is kept brief and never runs detection or session policy.
	// A try-lock here would silently drop valid operator commands under ordinary
	// worker contention, even when the bounded queue still has capacity.
	std::unique_lock<std::mutex> lock(queue_mutex);
	if (closing || closing_requested.load(std::memory_order_acquire)) {
		rejected_work.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	// Producers capture timestamps before acquiring this mutex. A delayed
	// producer must not place an older event after newer accepted work, because
	// the Rust session rejects non-monotonic events and status age would regress.
	// When timestamps tie, this mutex defines the accepted FIFO order; Rust
	// advances due timers before each queued event. Coalescing replaces only the
	// adjacent tail item and keeps that queue position.
	item.monotonic_ms = std::max(item.monotonic_ms, last_submitted_ms);

	if (coalesce && queue_size) {
		const size_t tail = (queue_head + queue_size - 1) % queue.size();
		if (queue[tail].kind == item.kind) {
			queue[tail] = item;
			accepted_work.fetch_add(1, std::memory_order_relaxed);
			coalesced_work.fetch_add(1, std::memory_order_relaxed);
			last_submitted_ms = std::max(last_submitted_ms, item.monotonic_ms);
			lock.unlock();
			queue_condition.notify_one();
			return true;
		}
	}
	// Keep one slot reserved so shutdown can always enqueue its final lifecycle event.
	if (queue_size == max_queued_work) {
		rejected_work.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const size_t tail = (queue_head + queue_size) % queue.size();
	queue[tail] = item;
	++queue_size;
	queue_depth.store(queue_size, std::memory_order_relaxed);
	update_max(queue_high_water, queue_size);
	accepted_work.fetch_add(1, std::memory_order_relaxed);
	last_submitted_ms = std::max(last_submitted_ms, item.monotonic_ms);
	lock.unlock();
	queue_condition.notify_one();
	return true;
}

bool faithfulframe_session_executor::try_snapshot(ff_snapshot &output, uint64_t *published_at_ms) const
{
	if (!published.try_read(output, published_at_ms))
		return false;
	if (output.closed)
		return true;
	return worker_started.load(std::memory_order_acquire) && !closing_requested.load(std::memory_order_acquire);
}

ff_executor_diagnostics faithfulframe_session_executor::diagnostics() const
{
	return ff_executor_diagnostics{
		queue_depth.load(std::memory_order_relaxed),       queue_high_water.load(std::memory_order_relaxed),
		accepted_work.load(std::memory_order_relaxed),     rejected_work.load(std::memory_order_relaxed),
		coalesced_work.load(std::memory_order_relaxed),    last_queue_wait_us.load(std::memory_order_relaxed),
		max_queue_wait_us.load(std::memory_order_relaxed), last_processing_us.load(std::memory_order_relaxed),
		max_processing_us.load(std::memory_order_relaxed)};
}

void faithfulframe_session_executor::request_shutdown(uint32_t terminal_lifecycle_kind, uint32_t source_width,
						      uint32_t source_height)
{
	std::lock_guard<std::mutex> request_lock(shutdown_request_mutex);
	if (closing_requested.load(std::memory_order_acquire))
		return;
	closing_requested.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		closing = true;
		// Pending detector results and operator commands are neutralized. The
		// worker will derive the destroy generation from Rust's latest state.
		queue_head = 0;
		queue_size = 0;
		queue_depth.store(0, std::memory_order_relaxed);
		if (worker_started.load(std::memory_order_acquire)) {
			work_item stop;
			stop.kind = work_kind::stop;
			stop.enqueued_at = std::chrono::steady_clock::now();
			stop.lifecycle.kind = terminal_lifecycle_kind;
			stop.lifecycle.source_width = source_width;
			stop.lifecycle.source_height = source_height;
			stop.monotonic_ms = last_submitted_ms == std::numeric_limits<uint64_t>::max()
						    ? last_submitted_ms
						    : last_submitted_ms + 1;
			queue[0] = stop;
			queue_size = 1;
			queue_depth.store(1, std::memory_order_relaxed);
			update_max(queue_high_water, 1);
		}
	}
	queue_condition.notify_one();
}

void faithfulframe_session_executor::shutdown(uint32_t terminal_lifecycle_kind, uint32_t source_width,
					      uint32_t source_height)
{
	request_shutdown(terminal_lifecycle_kind, source_width, source_height);
	join();
}

void faithfulframe_session_executor::join()
{
	std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex);
	if (worker.joinable())
		worker.join();
	worker_started.store(false, std::memory_order_release);
}

class faithfulframe_executor_reaper {
public:
	faithfulframe_executor_reaper()
	{
		try {
			worker = std::thread(&faithfulframe_executor_reaper::run, this);
		} catch (...) {
		}
	}

	~faithfulframe_executor_reaper() { shutdown(); }

	bool available() const { return worker.joinable(); }

	bool enqueue(const std::shared_ptr<faithfulframe_session_executor> &executor)
	{
		if (!executor || !worker.joinable())
			return false;
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping)
			return false;
		if (executor->retirement_queued)
			return true;

		// The executor owns itself until the worker moves this reference into a
		// local variable. No std::function/deque allocation can fail here.
		executor->retirement_keepalive = executor;
		executor->retirement_queued = true;
		executor->retirement_next = nullptr;
		if (tail)
			tail->retirement_next = executor.get();
		else
			head = executor.get();
		tail = executor.get();
		condition.notify_one();
		return true;
	}

	void shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		condition.notify_one();
		if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
			worker.join();
	}

private:
	void run()
	{
		for (;;) {
			std::shared_ptr<faithfulframe_session_executor> executor;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] { return stopping || head; });
				if (!head) {
					if (stopping)
						return;
					continue;
				}
				auto *node = head;
				head = node->retirement_next;
				if (!head)
					tail = nullptr;
				node->retirement_next = nullptr;
				executor = std::move(node->retirement_keepalive);
			}
			if (executor)
				executor->join();
		}
	}

	std::mutex mutex;
	std::condition_variable condition;
	faithfulframe_session_executor *head = nullptr;
	faithfulframe_session_executor *tail = nullptr;
	bool stopping = false;
	std::thread worker;
};

namespace {
std::mutex faithfulframe_reaper_owner_mutex;
std::unique_ptr<faithfulframe_executor_reaper> faithfulframe_reaper;
bool faithfulframe_reaper_closed = false;
std::mutex faithfulframe_module_lifecycle_mutex;
size_t faithfulframe_live_instances = 0;
bool faithfulframe_module_unloading = false;
bool faithfulframe_reaper_shutdown_started = false;
} // namespace

bool faithfulframe_retire_session_executor(std::shared_ptr<faithfulframe_session_executor> &executor)
{
	if (!executor)
		return true;
	std::lock_guard<std::mutex> lock(faithfulframe_reaper_owner_mutex);
	// The reaper is created during module load. Creating threads or allocating
	// queue nodes from an OBS destruction callback would reintroduce a blocking
	// teardown path, so a missing/closed reaper is treated as a lifecycle
	// contract violation and left to the caller's explicit failure path.
	if (faithfulframe_reaper_closed || !faithfulframe_reaper)
		return false;
	if (!faithfulframe_reaper->enqueue(executor))
		return false;
	executor.reset();
	return true;
}

extern "C" int faithfulframe_executor_reaper_start()
{
	std::lock_guard<std::mutex> lock(faithfulframe_reaper_owner_mutex);
	faithfulframe_reaper_closed = false;
	if (!faithfulframe_reaper || !faithfulframe_reaper->available()) {
		faithfulframe_reaper.reset();
		try {
			faithfulframe_reaper = std::make_unique<faithfulframe_executor_reaper>();
		} catch (...) {
			faithfulframe_reaper_closed = true;
			return 0;
		}
	}
	if (!faithfulframe_reaper || !faithfulframe_reaper->available()) {
		faithfulframe_reaper.reset();
		faithfulframe_reaper_closed = true;
		return 0;
	}
	{
		std::lock_guard<std::mutex> lifecycle_lock(faithfulframe_module_lifecycle_mutex);
		faithfulframe_live_instances = 0;
		faithfulframe_module_unloading = false;
		faithfulframe_reaper_shutdown_started = false;
	}
	return 1;
}

extern "C" void faithfulframe_executor_reaper_shutdown()
{
	std::unique_ptr<faithfulframe_executor_reaper> reaper;
	{
		std::lock_guard<std::mutex> lock(faithfulframe_reaper_owner_mutex);
		faithfulframe_reaper_closed = true;
		reaper = std::move(faithfulframe_reaper);
	}
	if (reaper)
		reaper->shutdown();
}

extern "C" int faithfulframe_teardown_register_instance()
{
	std::lock_guard<std::mutex> lock(faithfulframe_module_lifecycle_mutex);
	if (faithfulframe_module_unloading || faithfulframe_reaper_shutdown_started ||
	    faithfulframe_live_instances == std::numeric_limits<size_t>::max())
		return 0;
	++faithfulframe_live_instances;
	return 1;
}

extern "C" void faithfulframe_teardown_instance_destroyed()
{
	bool shutdown_reaper = false;
	{
		std::lock_guard<std::mutex> lock(faithfulframe_module_lifecycle_mutex);
		if (faithfulframe_live_instances == 0)
			return;
		--faithfulframe_live_instances;
		if (faithfulframe_module_unloading && faithfulframe_live_instances == 0 &&
		    !faithfulframe_reaper_shutdown_started) {
			faithfulframe_reaper_shutdown_started = true;
			shutdown_reaper = true;
		}
	}
	if (shutdown_reaper)
		faithfulframe_executor_reaper_shutdown();
}

extern "C" void faithfulframe_teardown_begin_module_unload()
{
	bool shutdown_reaper = false;
	{
		std::lock_guard<std::mutex> lock(faithfulframe_module_lifecycle_mutex);
		faithfulframe_module_unloading = true;
		if (faithfulframe_live_instances == 0 && !faithfulframe_reaper_shutdown_started) {
			faithfulframe_reaper_shutdown_started = true;
			shutdown_reaper = true;
		}
	}
	if (shutdown_reaper)
		faithfulframe_executor_reaper_shutdown();
}

void faithfulframe_session_executor::snapshot_publisher::publish(const ff_snapshot &snapshot, uint64_t published_at)
{
	std::array<uint64_t, word_count> copy{};
	std::memcpy(copy.data(), &snapshot, sizeof(snapshot));
	sequence.fetch_add(1, std::memory_order_seq_cst);
	for (size_t i = 0; i < word_count; ++i)
		words[i].store(copy[i], std::memory_order_seq_cst);
	published_at_ms.store(published_at, std::memory_order_seq_cst);
	sequence.fetch_add(1, std::memory_order_seq_cst);
}

bool faithfulframe_session_executor::snapshot_publisher::try_read(ff_snapshot &snapshot, uint64_t *published_at) const
{
	const uint64_t before = sequence.load(std::memory_order_seq_cst);
	if (before == 0 || (before & 1))
		return false;
	std::array<uint64_t, word_count> copy{};
	for (size_t i = 0; i < word_count; ++i)
		copy[i] = words[i].load(std::memory_order_seq_cst);
	const uint64_t time = published_at_ms.load(std::memory_order_seq_cst);
	if (sequence.load(std::memory_order_seq_cst) != before)
		return false;
	std::memcpy(&snapshot, copy.data(), sizeof(snapshot));
	if (published_at)
		*published_at = time;
	return true;
}

void faithfulframe_session_executor::run()
{
	auto mark_stopped = [this] {
		closing_requested.store(true, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(queue_mutex);
			closing = true;
			queue_head = 0;
			queue_size = 0;
			queue_depth.store(0, std::memory_order_relaxed);
		}
		worker_started.store(false, std::memory_order_release);
		queue_condition.notify_all();
	};
	try {
		ff_result result = FF_OK;
		faithfulframe_core_bridge bridge(config, identity, initial_generations, result);
		if (result != FF_OK) {
			mark_stopped();
			return;
		}

		ff_snapshot snapshot{};
		if (bridge.snapshot(snapshot) != FF_OK) {
			mark_stopped();
			return;
		}
		published.publish(snapshot, 0);
		uint64_t command_message_expires_at = 0;
		uint32_t last_command_result = FF_COMMAND_NOT_APPLICABLE;
		uint32_t last_rejection = FF_REJECTION_NONE;

		for (;;) {
			work_item item;
			{
				std::unique_lock<std::mutex> lock(queue_mutex);
				queue_condition.wait(lock, [this] { return queue_size != 0 || closing; });
				if (!queue_size)
					break;
				item = queue[queue_head];
				queue_head = (queue_head + 1) % queue.size();
				--queue_size;
				queue_depth.store(queue_size, std::memory_order_relaxed);
			}
			last_queue_wait_us.store(elapsed_microseconds(item.enqueued_at), std::memory_order_relaxed);
			update_max(max_queue_wait_us, last_queue_wait_us.load(std::memory_order_relaxed));
			const auto processing_started = std::chrono::steady_clock::now();

			if (item.kind == work_kind::stop) {
				ff_lifecycle destroyed{};
				destroyed.kind = item.lifecycle.kind;
				destroyed.identity = identity;
				destroyed.source_width = item.lifecycle.source_width;
				destroyed.source_height = item.lifecycle.source_height;
				const bool can_advance = faithfulframe_next_lifecycle_generations(
					destroyed.kind, snapshot.generations, destroyed.generations);
				if (can_advance && bridge.lifecycle(item.monotonic_ms, destroyed, snapshot) == FF_OK) {
					snapshot.command_result = FF_COMMAND_NOT_APPLICABLE;
					snapshot.rejection_reason = FF_REJECTION_NONE;
					published.publish(snapshot, item.monotonic_ms);
				}
				last_processing_us.store(elapsed_microseconds(processing_started),
							 std::memory_order_relaxed);
				update_max(max_processing_us, last_processing_us.load(std::memory_order_relaxed));
				break;
			}

			ff_result call_result = FF_OK;
			switch (item.kind) {
			case work_kind::tick:
				call_result = bridge.tick(item.monotonic_ms, snapshot);
				break;
			case work_kind::candidates:
				if (!item.candidate_count) {
					call_result = bridge.clear_candidates(item.monotonic_ms,
									      item.candidate_identity,
									      item.candidate_generations,
									      item.observation_version, snapshot);
				} else {
					for (size_t i = 0; i < item.candidate_count && call_result == FF_OK; ++i)
						call_result = bridge.publish_candidate(item.monotonic_ms,
										       item.candidates[i], snapshot);
				}
				break;
			case work_kind::command:
				call_result = bridge.apply_command(item.monotonic_ms, item.command, snapshot);
				break;
			case work_kind::lifecycle:
				call_result = bridge.lifecycle(item.monotonic_ms, item.lifecycle, snapshot);
				break;
			case work_kind::stop:
				break;
			}
			if (call_result != FF_OK) {
				// A valid executor owns a valid session, so a bridge error means the
				// policy result cannot be trusted. Stop publishing snapshots; the OBS
				// adapter will detect the failed worker and use its safe-wide fallback.
				mark_stopped();
				return;
			}
			if (call_result == FF_OK) {
				if (item.kind == work_kind::command &&
				    snapshot.command_result != FF_COMMAND_NOT_APPLICABLE) {
					last_command_result = snapshot.command_result;
					last_rejection = snapshot.command_result == FF_COMMAND_REJECTED
								 ? snapshot.rejection_reason
								 : static_cast<uint32_t>(FF_REJECTION_NONE);
					const uint64_t lifetime = snapshot.command_result == FF_COMMAND_REJECTED ? 2000
														 : 500;
					command_message_expires_at =
						item.monotonic_ms > std::numeric_limits<uint64_t>::max() - lifetime
							? std::numeric_limits<uint64_t>::max()
							: item.monotonic_ms + lifetime;
				} else if (item.kind == work_kind::command || item.kind == work_kind::lifecycle) {
					last_command_result = FF_COMMAND_NOT_APPLICABLE;
					last_rejection = FF_REJECTION_NONE;
					command_message_expires_at = 0;
				}
				if (last_command_result != FF_COMMAND_NOT_APPLICABLE &&
				    item.monotonic_ms < command_message_expires_at) {
					snapshot.command_result = last_command_result;
					snapshot.rejection_reason = last_rejection;
				} else {
					last_command_result = FF_COMMAND_NOT_APPLICABLE;
					last_rejection = FF_REJECTION_NONE;
					command_message_expires_at = 0;
					snapshot.command_result = FF_COMMAND_NOT_APPLICABLE;
					snapshot.rejection_reason = FF_REJECTION_NONE;
				}
				published.publish(snapshot, item.monotonic_ms);
			}
			last_processing_us.store(elapsed_microseconds(processing_started), std::memory_order_relaxed);
			update_max(max_processing_us, last_processing_us.load(std::memory_order_relaxed));
			const bool terminal_lifecycle = item.kind == work_kind::lifecycle &&
							(item.lifecycle.kind == FF_LIFECYCLE_SOURCE_DESTROYED ||
							 item.lifecycle.kind == FF_LIFECYCLE_SOURCE_REBOUND ||
							 item.lifecycle.kind == FF_LIFECYCLE_ADAPTER_DESTROYED ||
							 item.lifecycle.kind == FF_LIFECYCLE_OBS_SHUTDOWN);
			if (terminal_lifecycle) {
				{
					std::lock_guard<std::mutex> lock(queue_mutex);
					closing_requested.store(true, std::memory_order_release);
					closing = true;
					queue_head = 0;
					queue_size = 0;
					queue_depth.store(0, std::memory_order_relaxed);
				}
				queue_condition.notify_all();
				break;
			}
		}
	} catch (...) {
		// Never allow a C++ exception to escape the worker entry point.
	}
	mark_stopped();
}
