#include "faithfulframe-core-bridge.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

#ifdef FAITHFULFRAME_TEST_REAPER_ENQUEUE_HOOK
namespace {
std::mutex faithfulframe_test_enqueue_hook_mutex;
void (*faithfulframe_test_enqueue_hook)(void *) = nullptr;
void *faithfulframe_test_enqueue_hook_context = nullptr;

void faithfulframe_test_run_reaper_enqueue_hook()
{
	void (*hook)(void *) = nullptr;
	void *context = nullptr;
	{
		std::lock_guard<std::mutex> lock(faithfulframe_test_enqueue_hook_mutex);
		hook = faithfulframe_test_enqueue_hook;
		context = faithfulframe_test_enqueue_hook_context;
	}
	if (hook)
		hook(context);
}
} // namespace
#endif

#ifdef FAITHFULFRAME_TEST_REAPER_QUEUE_LOCK_HOOK
namespace {
std::mutex faithfulframe_test_reaper_queue_lock_hook_mutex;
void (*faithfulframe_test_reaper_queue_lock_hook)(void *) = nullptr;
void *faithfulframe_test_reaper_queue_lock_hook_context = nullptr;

void faithfulframe_test_run_reaper_queue_lock_hook()
{
	void (*hook)(void *) = nullptr;
	void *context = nullptr;
	{
		std::lock_guard<std::mutex> lock(faithfulframe_test_reaper_queue_lock_hook_mutex);
		hook = faithfulframe_test_reaper_queue_lock_hook;
		context = faithfulframe_test_reaper_queue_lock_hook_context;
	}
	if (hook)
		hook(context);
}
} // namespace
#endif

#ifdef FAITHFULFRAME_TEST_REAPER_WAIT_HOOK
namespace {
std::mutex faithfulframe_test_reaper_wait_hook_mutex;
void (*faithfulframe_test_reaper_wait_hook)(void *) = nullptr;
void *faithfulframe_test_reaper_wait_hook_context = nullptr;

void faithfulframe_test_run_reaper_wait_hook()
{
	void (*hook)(void *) = nullptr;
	void *context = nullptr;
	{
		std::lock_guard<std::mutex> lock(faithfulframe_test_reaper_wait_hook_mutex);
		hook = faithfulframe_test_reaper_wait_hook;
		context = faithfulframe_test_reaper_wait_hook_context;
	}
	if (hook)
		hook(context);
}
} // namespace
#endif

#ifdef FAITHFULFRAME_TEST_REAPER_SHUTDOWN_HOOK
namespace {
std::atomic<bool> faithfulframe_test_reaper_waiting_for_workers_flag{false};
} // namespace

extern "C" int faithfulframe_test_reaper_shutdown_waiting(void)
{
	return faithfulframe_test_reaper_waiting_for_workers_flag.load(std::memory_order_acquire) ? 1 : 0;
}
#endif

#ifdef FAITHFULFRAME_TEST_REAPER_JOIN_HOOK
namespace {
std::mutex faithfulframe_test_join_hook_mutex;
void (*faithfulframe_test_join_hook)(void *) = nullptr;
void *faithfulframe_test_join_hook_context = nullptr;

void faithfulframe_test_run_reaper_join_hook()
{
	void (*hook)(void *) = nullptr;
	void *context = nullptr;
	{
		std::lock_guard<std::mutex> lock(faithfulframe_test_join_hook_mutex);
		hook = faithfulframe_test_join_hook;
		context = faithfulframe_test_join_hook_context;
	}
	if (hook)
		hook(context);
}
} // namespace
#endif

#ifdef FAITHFULFRAME_TEST_SESSION_WORK_HOOK
namespace {
std::mutex faithfulframe_test_session_work_hook_mutex;
void (*faithfulframe_test_session_work_hook)(void *) = nullptr;
void *faithfulframe_test_session_work_hook_context = nullptr;

void faithfulframe_test_run_session_work_hook()
{
	void (*hook)(void *) = nullptr;
	void *context = nullptr;
	{
		std::lock_guard<std::mutex> lock(faithfulframe_test_session_work_hook_mutex);
		hook = faithfulframe_test_session_work_hook;
		context = faithfulframe_test_session_work_hook_context;
	}
	if (hook)
		hook(context);
}
} // namespace
#endif

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

static uint64_t steady_timestamp_nanoseconds(std::chrono::steady_clock::time_point time)
{
	const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
	return timestamp > 0 ? static_cast<uint64_t>(timestamp) : 0;
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

static bool faithfulframe_reserve_reaper_worker(const std::shared_ptr<faithfulframe_executor_reaper> &reaper);
static void faithfulframe_release_reaper_worker(const std::shared_ptr<faithfulframe_executor_reaper> &reaper);

bool faithfulframe_next_lifecycle_generations(uint32_t kind, const ff_generations &current, ff_generations &next)
{
	next = current;
	return ff_lifecycle_next_generations(kind, &current, &next) == FF_OK;
}

faithfulframe_session_executor::faithfulframe_session_executor(const ff_session_config &config_,
							       const ff_identity &identity_,
							       const ff_generations &generations)
	: config(config_),
	  identity(identity_),
	  initial_generations(generations),
	  reaper(faithfulframe_acquire_executor_reaper())
{
	if (!reaper || !faithfulframe_reserve_reaper_worker(reaper))
		return;
	reaper_worker_reserved = true;
	worker_started.store(true, std::memory_order_release);
	try {
		worker = std::thread(&faithfulframe_session_executor::run, this);
	} catch (...) {
		worker_started.store(false, std::memory_order_release);
		reaper_worker_reserved = false;
		faithfulframe_release_reaper_worker(reaper);
	}
}

faithfulframe_session_executor::~faithfulframe_session_executor()
{
	shutdown();
	if (reaper_worker_reserved) {
		reaper_worker_reserved = false;
		faithfulframe_release_reaper_worker(reaper);
	}
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

bool faithfulframe_session_executor::enqueue_command(uint64_t monotonic_ms, const ff_command &command, bool *queue_full)
{
	work_item item;
	item.kind = work_kind::command;
	item.monotonic_ms = monotonic_ms;
	item.command = command;
	return enqueue(item, false, queue_full);
}

bool faithfulframe_session_executor::enqueue_lifecycle(uint64_t monotonic_ms, const ff_lifecycle &lifecycle)
{
	work_item item;
	item.kind = work_kind::lifecycle;
	item.monotonic_ms = monotonic_ms;
	item.lifecycle = lifecycle;
	return enqueue(item, false);
}

bool faithfulframe_session_executor::enqueue(work_item item, bool coalesce, bool *queue_full)
{
	if (queue_full)
		*queue_full = false;
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
			update_oldest_queue_timestamp_locked();
			accepted_work.fetch_add(1, std::memory_order_relaxed);
			coalesced_work.fetch_add(1, std::memory_order_relaxed);
			last_submitted_ms = std::max(last_submitted_ms, item.monotonic_ms);
			lock.unlock();
			queue_condition.notify_one();
			return true;
		}
	}
	if (item.kind == work_kind::tick || item.kind == work_kind::candidates) {
		size_t queued_background_work = 0;
		for (size_t offset = 0; offset < queue_size; ++offset) {
			const work_kind queued_kind = queue[(queue_head + offset) % queue.size()].kind;
			if (queued_kind == work_kind::tick || queued_kind == work_kind::candidates)
				++queued_background_work;
		}
		// Bound the tick/candidate backlog below total capacity so operator and
		// lifecycle events have reserved queue space under detector load.
		if (queued_background_work >= max_queued_background_work) {
			if (queue_full)
				*queue_full = true;
			rejected_work.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}
	if (queue_size == max_queued_work) {
		if (queue_full)
			*queue_full = true;
		rejected_work.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const size_t tail = (queue_head + queue_size) % queue.size();
	queue[tail] = item;
	++queue_size;
	queue_depth.store(queue_size, std::memory_order_relaxed);
	update_oldest_queue_timestamp_locked();
	update_max(queue_high_water, queue_size);
	accepted_work.fetch_add(1, std::memory_order_relaxed);
	last_submitted_ms = std::max(last_submitted_ms, item.monotonic_ms);
	lock.unlock();
	queue_condition.notify_one();
	return true;
}

void faithfulframe_session_executor::update_oldest_queue_timestamp_locked()
{
	const uint64_t timestamp = queue_size ? steady_timestamp_nanoseconds(queue[queue_head].enqueued_at) : 0;
	oldest_queued_at_ns.store(timestamp, std::memory_order_release);
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
	const uint64_t oldest_queued_at = oldest_queued_at_ns.load(std::memory_order_acquire);
	const uint64_t now = steady_timestamp_nanoseconds(std::chrono::steady_clock::now());
	const uint64_t oldest_queue_age_us =
		oldest_queued_at && now > oldest_queued_at ? (now - oldest_queued_at) / 1000 : 0;
	return ff_executor_diagnostics{queue_depth.load(std::memory_order_relaxed),
				       queue_high_water.load(std::memory_order_relaxed),
				       oldest_queue_age_us,
				       accepted_work.load(std::memory_order_relaxed),
				       rejected_work.load(std::memory_order_relaxed),
				       coalesced_work.load(std::memory_order_relaxed),
				       last_queue_wait_us.load(std::memory_order_relaxed),
				       max_queue_wait_us.load(std::memory_order_relaxed),
				       last_processing_us.load(std::memory_order_relaxed),
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
			update_oldest_queue_timestamp_locked();
			update_max(queue_high_water, 1);
		}
		if (!queue_size)
			update_oldest_queue_timestamp_locked();
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
	bool release_reaper_reservation = false;
	{
		std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex);
		if (worker.joinable())
			worker.join();
		worker_started.store(false, std::memory_order_release);
		if (reaper_worker_reserved) {
			reaper_worker_reserved = false;
			release_reaper_reservation = true;
		}
	}
	if (release_reaper_reservation)
		faithfulframe_release_reaper_worker(reaper);
}

class faithfulframe_executor_reaper {
public:
	faithfulframe_executor_reaper()
	{
		try {
			worker = std::thread(&faithfulframe_executor_reaper::run, this);
			accepting.store(true, std::memory_order_release);
		} catch (...) {
		}
	}

	~faithfulframe_executor_reaper() { shutdown(); }

	bool available() const { return accepting.load(std::memory_order_acquire); }

	bool reserve_worker()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!accepting.load(std::memory_order_acquire) || stopping)
			return false;
		if (active_executor_workers == std::numeric_limits<size_t>::max())
			return false;
		++active_executor_workers;
		return true;
	}

	void release_worker()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (active_executor_workers > 0)
				--active_executor_workers;
		}
		condition.notify_all();
	}

	bool enqueue(const std::shared_ptr<faithfulframe_session_executor> &executor, bool wait_for_lock = false)
	{
		if (!executor) {
			return false;
		}
		// Keep the test gate outside the queue critical section. A delayed caller
		// must not prevent independent sessions from handing off their workers.
#ifdef FAITHFULFRAME_TEST_REAPER_ENQUEUE_HOOK
		faithfulframe_test_run_reaper_enqueue_hook();
#endif
#ifdef FAITHFULFRAME_TEST_REAPER_WAIT_HOOK
		if (wait_for_lock)
			faithfulframe_test_run_reaper_wait_hook();
#endif
		{
			std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
			if (wait_for_lock)
				lock.lock();
			else if (!lock.try_lock())
				return false;
			if (!accepting.load(std::memory_order_acquire))
				return false;
			if (executor->retirement_queued)
				return true;
#ifdef FAITHFULFRAME_TEST_REAPER_QUEUE_LOCK_HOOK
			faithfulframe_test_run_reaper_queue_lock_hook();
#endif
			executor->retirement_keepalive = executor;
			executor->retirement_queued = true;
			executor->reap_task.callback = &faithfulframe_executor_reaper::join_executor;
			executor->reap_task.context = executor.get();
			append_locked(&executor->reap_task, true);
		}
		condition.notify_one();
		return true;
	}

	bool enqueue_cleanup(faithfulframe_reap_task *task, void (*callback)(void *), void *context,
			     bool wait_for_lock = false)
	{
		if (!task || !callback)
			return false;
#ifdef FAITHFULFRAME_TEST_REAPER_WAIT_HOOK
		if (wait_for_lock)
			faithfulframe_test_run_reaper_wait_hook();
#endif
		{
			std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
			if (wait_for_lock)
				lock.lock();
			else if (!lock.try_lock())
				return false;
			if (!accepting.load(std::memory_order_acquire) || task->queued)
				return false;
			task->callback = callback;
			task->context = context;
			append_locked(task, false);
		}
		return true;
	}

	void shutdown()
	{
		std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex);
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (active_executor_workers != 0 || head || task_in_progress) {
#ifdef FAITHFULFRAME_TEST_REAPER_SHUTDOWN_HOOK
				faithfulframe_test_reaper_waiting_for_workers_flag.store(true,
											 std::memory_order_release);
#endif
				condition.wait(lock, [this] {
					return active_executor_workers == 0 && !head && !task_in_progress;
				});
#ifdef FAITHFULFRAME_TEST_REAPER_SHUTDOWN_HOOK
				faithfulframe_test_reaper_waiting_for_workers_flag.store(false,
											 std::memory_order_release);
#endif
			}
			accepting.store(false, std::memory_order_release);
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
			faithfulframe_reap_task *task = nullptr;
			void (*callback)(void *) = nullptr;
			void *context = nullptr;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this] {
					return (head && head->ready.load(std::memory_order_acquire)) ||
					       (stopping && !head);
				});
				if (!head) {
					if (stopping)
						return;
					continue;
				}
				task = head;
				head = task->next;
				if (!head)
					tail = nullptr;
				task->next = nullptr;
				callback = task->callback;
				context = task->context;
				task_in_progress = true;
			}
			if (callback)
				callback(context);
			{
				std::lock_guard<std::mutex> lock(mutex);
				task_in_progress = false;
			}
			condition.notify_all();
		}
	}

private:
	static void join_executor(void *context)
	{
		auto *executor = static_cast<faithfulframe_session_executor *>(context);
		// Keep the embedded task node alive until this callback returns, then drop
		// the reaper's self-reference after the worker has been joined.
		auto keepalive = std::move(executor->retirement_keepalive);
		if (keepalive) {
#ifdef FAITHFULFRAME_TEST_REAPER_JOIN_HOOK
			faithfulframe_test_run_reaper_join_hook();
#endif
			keepalive->join();
		}
	}

	void append_locked(faithfulframe_reap_task *task, bool ready)
	{
		task->next = nullptr;
		task->wake_reaper = &faithfulframe_executor_reaper::notify_worker;
		task->wake_context = this;
		task->ready.store(ready, std::memory_order_relaxed);
		task->queued = true;
		if (tail)
			tail->next = task;
		else
			head = task;
		tail = task;
	}

	static void notify_worker(void *context)
	{
		static_cast<faithfulframe_executor_reaper *>(context)->condition.notify_one();
	}

	std::mutex shutdown_mutex;
	std::mutex mutex;
	std::condition_variable condition;
	faithfulframe_reap_task *head = nullptr;
	faithfulframe_reap_task *tail = nullptr;
	bool stopping = false;
	bool task_in_progress = false;
	size_t active_executor_workers = 0;
	std::atomic<bool> accepting{false};
	std::thread worker;
};

static bool faithfulframe_reserve_reaper_worker(const std::shared_ptr<faithfulframe_executor_reaper> &reaper)
{
	return reaper && reaper->reserve_worker();
}

static void faithfulframe_release_reaper_worker(const std::shared_ptr<faithfulframe_executor_reaper> &reaper)
{
	if (reaper)
		reaper->release_worker();
}

namespace {
std::mutex faithfulframe_reaper_owner_mutex;
std::shared_ptr<faithfulframe_executor_reaper> faithfulframe_reaper;
bool faithfulframe_reaper_closed = false;
std::mutex faithfulframe_module_lifecycle_mutex;
size_t faithfulframe_pending_creates = 0;
size_t faithfulframe_live_instances = 0;
bool faithfulframe_module_unloading = false;
bool faithfulframe_reaper_shutdown_started = false;
} // namespace

std::shared_ptr<faithfulframe_executor_reaper> faithfulframe_acquire_executor_reaper()
{
	std::unique_lock<std::mutex> lock(faithfulframe_reaper_owner_mutex, std::try_to_lock);
	if (!lock.owns_lock() || faithfulframe_reaper_closed || !faithfulframe_reaper ||
	    !faithfulframe_reaper->available())
		return {};
	return faithfulframe_reaper;
}

bool faithfulframe_defer_reap_task(faithfulframe_reap_task *task, void (*callback)(void *), void *context,
				   bool wait_for_reaper)
{
	std::shared_ptr<faithfulframe_executor_reaper> reaper;
	if (wait_for_reaper) {
		std::lock_guard<std::mutex> lock(faithfulframe_reaper_owner_mutex);
		if (!faithfulframe_reaper_closed && faithfulframe_reaper && faithfulframe_reaper->available())
			reaper = faithfulframe_reaper;
	} else {
		reaper = faithfulframe_acquire_executor_reaper();
	}
	return reaper && reaper->enqueue_cleanup(task, callback, context, wait_for_reaper);
}

void faithfulframe_activate_reap_task(faithfulframe_reap_task *task)
{
	if (!task)
		return;
	void (*wake)(void *) = task->wake_reaper;
	void *wake_context = task->wake_context;
	bool expected_ready = false;
	if (task->ready.compare_exchange_strong(expected_ready, true, std::memory_order_release,
						std::memory_order_relaxed) &&
	    wake)
		wake(wake_context);
}

bool faithfulframe_retire_session_executor(std::shared_ptr<faithfulframe_session_executor> &executor,
					   bool wait_for_reaper)
{
	if (!executor)
		return true;
	if (!executor->reaper || !executor->reaper->enqueue(executor, wait_for_reaper))
		return false;
	executor.reset();
	return true;
}

extern "C" int faithfulframe_executor_reaper_start()
{
	std::lock_guard<std::mutex> lock(faithfulframe_reaper_owner_mutex);
	const bool reaper_available = faithfulframe_reaper && faithfulframe_reaper->available();
	{
		std::lock_guard<std::mutex> lifecycle_lock(faithfulframe_module_lifecycle_mutex);
		if (reaper_available) {
			// Module load may be retried while an earlier generation is still
			// draining OBS source instances. Preserve its reservations and do not
			// reopen a module that has already begun unloading.
			return faithfulframe_module_unloading ? 0 : 1;
		}
		// A stopped/missing reaper cannot be replaced while any source creation
		// or destruction still depends on the current lifecycle generation.
		if (faithfulframe_live_instances != 0 || faithfulframe_pending_creates != 0)
			return 0;
	}
	faithfulframe_reaper.reset();
	try {
		faithfulframe_reaper = std::make_shared<faithfulframe_executor_reaper>();
	} catch (...) {
		faithfulframe_reaper_closed = true;
		return 0;
	}
	if (!faithfulframe_reaper || !faithfulframe_reaper->available()) {
		faithfulframe_reaper.reset();
		faithfulframe_reaper_closed = true;
		return 0;
	}
	faithfulframe_reaper_closed = false;
	{
		std::lock_guard<std::mutex> lifecycle_lock(faithfulframe_module_lifecycle_mutex);
		faithfulframe_pending_creates = 0;
		faithfulframe_live_instances = 0;
		faithfulframe_module_unloading = false;
		faithfulframe_reaper_shutdown_started = false;
	}
	return 1;
}

extern "C" void faithfulframe_executor_reaper_shutdown()
{
	std::shared_ptr<faithfulframe_executor_reaper> reaper;
	{
		// Keep the global reaper published while it drains. Cleanup callbacks may
		// enqueue follow-up resource releases, and all of them must finish before
		// the reaper stops accepting work.
		std::lock_guard<std::mutex> owner_lock(faithfulframe_reaper_owner_mutex);
		{
			std::lock_guard<std::mutex> lifecycle_lock(faithfulframe_module_lifecycle_mutex);
			if (!faithfulframe_module_unloading || faithfulframe_live_instances != 0 ||
			    faithfulframe_pending_creates != 0 || !faithfulframe_reaper_shutdown_started)
				return;
		}
		if (faithfulframe_reaper_closed || !faithfulframe_reaper)
			return;
		reaper = faithfulframe_reaper;
	}
	if (reaper)
		reaper->shutdown();
	{
		std::lock_guard<std::mutex> owner_lock(faithfulframe_reaper_owner_mutex);
		if (faithfulframe_reaper == reaper) {
			faithfulframe_reaper_closed = true;
			faithfulframe_reaper.reset();
		}
	}
}

extern "C" int faithfulframe_teardown_register_instance()
{
	// Source instances are only allowed after module load has established the
	// nonblocking join worker. This guarantees every later binding teardown can
	// transfer its executor without creating threads or joining inline.
	std::lock_guard<std::mutex> reaper_lock(faithfulframe_reaper_owner_mutex);
	if (faithfulframe_reaper_closed || !faithfulframe_reaper || !faithfulframe_reaper->available())
		return 0;
	std::lock_guard<std::mutex> lock(faithfulframe_module_lifecycle_mutex);
	if (faithfulframe_module_unloading || faithfulframe_reaper_shutdown_started ||
	    faithfulframe_live_instances >= std::numeric_limits<size_t>::max() - faithfulframe_pending_creates)
		return 0;
	++faithfulframe_pending_creates;
	return 1;
}

#ifdef FAITHFULFRAME_TEST_REAPER_JOIN_HOOK
extern "C" void faithfulframe_test_set_reaper_join_hook(void (*hook)(void *), void *context)
{
	std::lock_guard<std::mutex> lock(faithfulframe_test_join_hook_mutex);
	faithfulframe_test_join_hook = hook;
	faithfulframe_test_join_hook_context = context;
}
#endif

#ifdef FAITHFULFRAME_TEST_REAPER_ENQUEUE_HOOK
extern "C" void faithfulframe_test_set_reaper_enqueue_hook(void (*hook)(void *), void *context)
{
	std::lock_guard<std::mutex> lock(faithfulframe_test_enqueue_hook_mutex);
	faithfulframe_test_enqueue_hook = hook;
	faithfulframe_test_enqueue_hook_context = context;
}
#endif

#ifdef FAITHFULFRAME_TEST_REAPER_QUEUE_LOCK_HOOK
extern "C" void faithfulframe_test_set_reaper_queue_lock_hook(void (*hook)(void *), void *context)
{
	std::lock_guard<std::mutex> lock(faithfulframe_test_reaper_queue_lock_hook_mutex);
	faithfulframe_test_reaper_queue_lock_hook = hook;
	faithfulframe_test_reaper_queue_lock_hook_context = context;
}
#endif

#ifdef FAITHFULFRAME_TEST_REAPER_WAIT_HOOK
extern "C" void faithfulframe_test_set_reaper_wait_hook(void (*hook)(void *), void *context)
{
	std::lock_guard<std::mutex> lock(faithfulframe_test_reaper_wait_hook_mutex);
	faithfulframe_test_reaper_wait_hook = hook;
	faithfulframe_test_reaper_wait_hook_context = context;
}
#endif

#ifdef FAITHFULFRAME_TEST_SESSION_WORK_HOOK
extern "C" void faithfulframe_test_set_session_work_hook(void (*hook)(void *), void *context)
{
	std::lock_guard<std::mutex> lock(faithfulframe_test_session_work_hook_mutex);
	faithfulframe_test_session_work_hook = hook;
	faithfulframe_test_session_work_hook_context = context;
}
#endif

extern "C" void faithfulframe_teardown_instance_created()
{
	std::lock_guard<std::mutex> lock(faithfulframe_module_lifecycle_mutex);
	// Every successful source/filter create has a matching reservation. Keeping
	// in-flight creates separate prevents a failed create on the UI path from
	// postponing the last real source's safe-thread executor-drain barrier.
	if (faithfulframe_pending_creates == 0 || faithfulframe_live_instances == std::numeric_limits<size_t>::max())
		return;
	--faithfulframe_pending_creates;
	++faithfulframe_live_instances;
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
		    faithfulframe_pending_creates == 0 && !faithfulframe_reaper_shutdown_started) {
			faithfulframe_reaper_shutdown_started = true;
			shutdown_reaper = true;
		}
	}
	if (shutdown_reaper)
		faithfulframe_executor_reaper_shutdown();
}

extern "C" void faithfulframe_teardown_instance_creation_failed()
{
	bool shutdown_reaper = false;
	{
		std::lock_guard<std::mutex> lock(faithfulframe_module_lifecycle_mutex);
		if (faithfulframe_pending_creates == 0)
			return;
		--faithfulframe_pending_creates;
		if (faithfulframe_module_unloading && faithfulframe_live_instances == 0 &&
		    faithfulframe_pending_creates == 0 && !faithfulframe_reaper_shutdown_started) {
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
		if (faithfulframe_live_instances == 0 && faithfulframe_pending_creates == 0 &&
		    !faithfulframe_reaper_shutdown_started) {
			faithfulframe_reaper_shutdown_started = true;
			shutdown_reaper = true;
		}
	}
	if (shutdown_reaper)
		faithfulframe_executor_reaper_shutdown();
}

void faithfulframe_session_executor::snapshot_publisher::publish(const ff_snapshot &snapshot, uint64_t published_at)
{
	const auto copy = faithfulframe_snapshot_words(snapshot);
	sequence.fetch_add(1, std::memory_order_seq_cst);
	for (size_t i = 0; i < faithfulframe_snapshot_word_count; ++i)
		words[i].store(copy[i], std::memory_order_seq_cst);
	published_at_ms.store(published_at, std::memory_order_seq_cst);
	sequence.fetch_add(1, std::memory_order_seq_cst);
}

bool faithfulframe_session_executor::snapshot_publisher::try_read(ff_snapshot &snapshot, uint64_t *published_at) const
{
	const uint64_t before = sequence.load(std::memory_order_seq_cst);
	if (before == 0 || (before & 1))
		return false;
	std::array<uint64_t, faithfulframe_snapshot_word_count> copy{};
	for (size_t i = 0; i < faithfulframe_snapshot_word_count; ++i)
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
			update_oldest_queue_timestamp_locked();
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
				update_oldest_queue_timestamp_locked();
			}
#ifdef FAITHFULFRAME_TEST_SESSION_WORK_HOOK
			faithfulframe_test_run_session_work_hook();
#endif
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
					update_oldest_queue_timestamp_locked();
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
