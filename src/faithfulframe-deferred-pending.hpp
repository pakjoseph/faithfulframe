#pragma once

#include <atomic>
#include <memory>

// A single-owner handoff slot for an object whose destruction must be deferred
// from a frame-critical producer. Producers keep ownership when the slot is
// occupied; consumers take ownership with one atomic exchange. The caller must
// serialize configuration attempts so a runtime has at most one unpublished
// binding waiting for reaper capacity.
template<typename Node> class faithfulframe_deferred_pending {
	static_assert(std::atomic<Node *>::is_always_lock_free,
		      "deferred reclamation must not lock the frame-critical producer");

public:
	bool try_store(std::unique_ptr<Node> &node) noexcept
	{
		if (!node)
			return false;
		Node *const raw = node.release();
		Node *empty = nullptr;
		if (pending.compare_exchange_strong(empty, raw, std::memory_order_release, std::memory_order_relaxed))
			return true;
		node.reset(raw);
		return false;
	}

	Node *take() noexcept { return pending.exchange(nullptr, std::memory_order_acquire); }

	bool occupied() const noexcept { return pending.load(std::memory_order_acquire) != nullptr; }

private:
	std::atomic<Node *> pending{nullptr};
};
